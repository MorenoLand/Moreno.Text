// Modification tracking: a line diff between the buffer and the file on disk drives the gutter
// markers, Next/Previous Modification and Revert Hunk.
#include "Core/Application.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(text.substr(start)); break; }
        size_t end = nl;
        if (end > start && text[end - 1] == '\r') --end;
        lines.push_back(text.substr(start, end - start));
        start = nl + 1;
    }
    return lines;
}

}  // namespace

// Fills `out` with hunks describing how to turn `oldLines` into `newLines`.
static void diffLines(const std::vector<std::string>& oldLines, const std::vector<std::string>& newLines, std::vector<DiffHunk>& out) {
    size_t prefix = 0;
    while (prefix < oldLines.size() && prefix < newLines.size() && oldLines[prefix] == newLines[prefix]) ++prefix;
    size_t suffix = 0;
    while (suffix < oldLines.size() - prefix && suffix < newLines.size() - prefix &&
           oldLines[oldLines.size() - 1 - suffix] == newLines[newLines.size() - 1 - suffix]) ++suffix;
    size_t oldLen = oldLines.size() - prefix - suffix, newLen = newLines.size() - prefix - suffix;
    if (oldLen == 0 && newLen == 0) return;
    // A single hunk when the middle is too large for an exact LCS.
    if (oldLen * newLen > 4000000) { out.push_back({prefix, prefix + newLen, prefix, prefix + oldLen}); return; }
    std::vector<std::vector<unsigned>> lcs(oldLen + 1, std::vector<unsigned>(newLen + 1, 0));
    for (size_t i = oldLen; i-- > 0;)
        for (size_t j = newLen; j-- > 0;)
            lcs[i][j] = oldLines[prefix + i] == newLines[prefix + j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
    size_t i = 0, j = 0;
    bool open = false;
    DiffHunk cur{};
    auto close = [&] {
        if (open) { out.push_back(cur); open = false; }
    };
    while (i < oldLen || j < newLen) {
        if (i < oldLen && j < newLen && oldLines[prefix + i] == newLines[prefix + j]) { close(); ++i; ++j; continue; }
        if (!open) { cur = {prefix + j, prefix + j, prefix + i, prefix + i}; open = true; }
        if (j < newLen && (i == oldLen || lcs[i][j + 1] >= lcs[i + 1][j])) { ++j; cur.newEnd = prefix + j; }
        else { ++i; cur.oldEnd = prefix + i; }
    }
    close();
}

void Application::refreshDiffHunks(bool force) {
    uint32_t now = SDL_GetTicks();
    if (!force && now - diffComputedAt_ < 350) return;
    diffComputedAt_ = now;
    diffHunks_.clear();
    if (!dirty_ || openFilePath_.empty() || largeFileGuarded_ || binaryView_) return;
    std::error_code ec;
    if (!fs::exists(openFilePath_, ec) || fs::file_size(openFilePath_, ec) > 2u * 1024u * 1024u) return;
    std::ifstream in(openFilePath_, std::ios::binary);
    if (!in) return;
    std::string disk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (disk.size() >= 3 && (unsigned char)disk[0] == 0xEF && (unsigned char)disk[1] == 0xBB && (unsigned char)disk[2] == 0xBF) disk.erase(0, 3);
    diffOldLines_ = splitLines(disk);
    diffLines(diffOldLines_, splitLines(textBuffer), diffHunks_);
}

void Application::drawModificationMarkers(float gutterRight, float textOriginY, float lineStep, float top, float bottom) {
    if (wordWrap_) return;
    refreshDiffHunks(false);
    if (diffHunks_.empty()) return;
    std::vector<float>& v = solidVerts_;
    auto rect = [&](float x0, float y0, float x1, float y1, float r, float g, float b) {
        y0 = std::max(y0, top); y1 = std::min(y1, bottom);
        if (y1 <= y0) return;
        v.insert(v.end(), {x0,y0,0,0,r,g,b,1, x0,y1,0,0,r,g,b,1, x1,y1,0,0,r,g,b,1, x0,y0,0,0,r,g,b,1, x1,y1,0,0,r,g,b,1, x1,y0,0,0,r,g,b,1});
    };
    for (const DiffHunk& h : diffHunks_) {
        float y0 = textOriginY + static_cast<float>(h.newStart) * lineStep - scrollY_;
        if (h.newEnd == h.newStart) {  // lines were removed here
            if (y0 > top - 6.f && y0 < bottom) rect(gutterRight - 5.f, y0 - 1.f, gutterRight, y0 + 2.f, 0.90f, 0.30f, 0.30f);
            continue;
        }
        float y1 = textOriginY + static_cast<float>(h.newEnd) * lineStep - scrollY_;
        if (y1 < top || y0 > bottom) continue;
        bool added = h.oldStart == h.oldEnd;
        if (added) rect(gutterRight - 3.f, y0, gutterRight, y1, 0.35f, 0.75f, 0.40f);
        else rect(gutterRight - 3.f, y0, gutterRight, y1, 0.95f, 0.70f, 0.25f);
    }
}

void Application::nextModification() {
    refreshDiffHunks(true);
    if (diffHunks_.empty() || selections_.empty()) return;
    size_t line = lineOfPos(selections_[0].cursor);
    for (const DiffHunk& h : diffHunks_) {
        if (h.newStart > line) { selections_.clear(); selections_.emplace_back(lineStartForLine(h.newStart)); ensureCursorVisible(); return; }
    }
    selections_.clear(); selections_.emplace_back(lineStartForLine(diffHunks_.front().newStart)); ensureCursorVisible();
}

void Application::previousModification() {
    refreshDiffHunks(true);
    if (diffHunks_.empty() || selections_.empty()) return;
    size_t line = lineOfPos(selections_[0].cursor);
    for (size_t i = diffHunks_.size(); i-- > 0;) {
        if (diffHunks_[i].newStart < line) { selections_.clear(); selections_.emplace_back(lineStartForLine(diffHunks_[i].newStart)); ensureCursorVisible(); return; }
    }
    selections_.clear(); selections_.emplace_back(lineStartForLine(diffHunks_.back().newStart)); ensureCursorVisible();
}

void Application::revertHunk() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    refreshDiffHunks(true);
    size_t line = lineOfPos(selections_[0].cursor);
    for (const DiffHunk& h : diffHunks_) {
        bool inside = line >= h.newStart && (line < h.newEnd || (h.newEnd == h.newStart && line == h.newStart));
        if (!inside) continue;
        std::vector<std::string> now = splitLines(textBuffer);
        std::vector<std::string> result(now.begin(), now.begin() + static_cast<std::ptrdiff_t>(h.newStart));
        result.insert(result.end(), diffOldLines_.begin() + static_cast<std::ptrdiff_t>(h.oldStart), diffOldLines_.begin() + static_cast<std::ptrdiff_t>(h.oldEnd));
        result.insert(result.end(), now.begin() + static_cast<std::ptrdiff_t>(h.newEnd), now.end());
        bool crlf = textBuffer.find("\r\n") != std::string::npos;
        std::string joined;
        for (size_t i = 0; i < result.size(); ++i) { if (i) joined += crlf ? "\r\n" : "\n"; joined += result[i]; }
        pushUndo();
        textBuffer = joined;
        selections_.clear(); selections_.emplace_back(std::min(textBuffer.size(), lineStartForLine(h.newStart)));
        invalidateLineIndex(); dirty_ = true; diffComputedAt_ = 0;
        return;
    }
}

void Application::revertAllModifications() {
    if (largeFileGuarded_ || binaryView_ || openFilePath_.empty()) return;
    revertFile();
    diffHunks_.clear();
}

void Application::cloneTab(size_t index) {
    saveCurrentTab();
    if (index >= tabs_.size() || tabs_[index].largeFileGuarded) return;
    TabBuffer copy = tabs_[index];
    copy.preview = false;
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(index) + 1, std::move(copy));
    for (auto& group : editorGroups_) if (group.tab > index) ++group.tab;
    if (activeTab_ > index) ++activeTab_;
    addEditorGroupForTab(index + 1);
}

void Application::diffWithTab(size_t otherIndex) {
    saveCurrentTab();
    if (otherIndex >= tabs_.size() || activeTab_ >= tabs_.size() || otherIndex == activeTab_) return;
    const TabBuffer& other = tabs_[otherIndex];
    const TabBuffer& current = tabs_[activeTab_];
    std::vector<std::string> a = splitLines(other.text), b = splitLines(current.text);
    std::vector<DiffHunk> hunks;
    diffLines(a, b, hunks);
    std::ostringstream out;
    out << "--- " << (other.fileName.empty() ? "untitled" : other.fileName) << "\n+++ " << (current.fileName.empty() ? "untitled" : current.fileName) << "\n";
    if (hunks.empty()) out << "\n(no differences)\n";
    for (const DiffHunk& h : hunks) {
        out << "@@ -" << h.oldStart + 1 << "," << (h.oldEnd - h.oldStart) << " +" << h.newStart + 1 << "," << (h.newEnd - h.newStart) << " @@\n";
        for (size_t i = h.oldStart; i < h.oldEnd; ++i) out << "-" << a[i] << "\n";
        for (size_t i = h.newStart; i < h.newEnd; ++i) out << "+" << b[i] << "\n";
    }
    newBuffer();
    textBuffer = out.str();
    invalidateLineIndex();
    openFile_ = "Diff";
    if (activeTab_ < tabs_.size()) tabs_[activeTab_].fileName = "Diff";
    dirty_ = false;
    selections_.clear(); selections_.emplace_back(0);
}
