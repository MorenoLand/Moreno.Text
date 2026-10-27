// Line editing, window and project commands reachable from the menus.
#include "Core/Application.h"
#include "Core/Version.h"
#include "Syntax/SyntaxHighlighter.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string leadingWhitespace(const std::string& text, size_t lineStartPos) {
    size_t i = lineStartPos;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    return text.substr(lineStartPos, i - lineStartPos);
}

void eraseRange(std::string& text, std::vector<SelRange>& sels, size_t a, size_t b) {
    if (b <= a) return;
    text.erase(a, b - a);
    sels.clear(); sels.emplace_back(a);
}

void openExternal(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + url + "' &";
    std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open '" + url + "' &";
    std::system(cmd.c_str());
#endif
}

}  // namespace

void Application::insertLineAfter() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    size_t le = lineEnd(selections_[0].cursor);
    std::string indent = leadingWhitespace(textBuffer, lineStart(selections_[0].cursor));
    textBuffer.insert(le, "\n" + indent);
    selections_.clear(); selections_.emplace_back(le + 1 + indent.size());
    invalidateLineIndex(); dirty_ = true; ensureCursorVisible();
}

void Application::insertLineBefore() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    size_t ls = lineStart(selections_[0].cursor);
    std::string indent = leadingWhitespace(textBuffer, ls);
    textBuffer.insert(ls, indent + "\n");
    selections_.clear(); selections_.emplace_back(ls + indent.size());
    invalidateLineIndex(); dirty_ = true; ensureCursorVisible();
}

void Application::deleteWordBackward() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    SelRange sel = selections_[0];
    pushUndo();
    size_t from = sel.hasSelection() ? sel.min() : sel.cursor, to = sel.hasSelection() ? sel.max() : sel.cursor;
    if (!sel.hasSelection()) {
        while (from > 0 && (textBuffer[from - 1] == ' ' || textBuffer[from - 1] == '\t')) --from;
        if (from > 0 && textBuffer[from - 1] == '\n') --from;
        else if (from > 0 && isWordChar(textBuffer[from - 1])) { while (from > 0 && isWordChar(textBuffer[from - 1])) --from; }
        else if (from > 0) --from;
    }
    eraseRange(textBuffer, selections_, from, to);
    invalidateLineIndex(); dirty_ = true;
}

void Application::deleteWordForward() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    SelRange sel = selections_[0];
    pushUndo();
    size_t from = sel.hasSelection() ? sel.min() : sel.cursor, to = sel.hasSelection() ? sel.max() : sel.cursor;
    if (!sel.hasSelection()) {
        while (to < textBuffer.size() && (textBuffer[to] == ' ' || textBuffer[to] == '\t')) ++to;
        if (to < textBuffer.size() && textBuffer[to] == '\n') ++to;
        else if (to < textBuffer.size() && isWordChar(textBuffer[to])) { while (to < textBuffer.size() && isWordChar(textBuffer[to])) ++to; }
        else if (to < textBuffer.size()) ++to;
    }
    eraseRange(textBuffer, selections_, from, to);
    invalidateLineIndex(); dirty_ = true;
}

void Application::deleteToLineEnd() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    size_t pos = selections_[0].cursor, le = lineEnd(pos);
    pushUndo();
    size_t killEnd = le == pos && le < textBuffer.size() ? le + 1 : le;
    if (killEnd > pos) killRing_ = textBuffer.substr(pos, killEnd - pos);
    eraseRange(textBuffer, selections_, pos, killEnd);
    invalidateLineIndex(); dirty_ = true;
}

void Application::deleteToLineStart() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    size_t pos = selections_[0].cursor, ls = lineStart(pos);
    pushUndo();
    eraseRange(textBuffer, selections_, ls, pos);
    invalidateLineIndex(); dirty_ = true;
}

void Application::transposeCharacters() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty() || selections_[0].hasSelection()) return;
    size_t pos = selections_[0].cursor;
    if (pos < textBuffer.size() && textBuffer[pos] == '\n') { if (pos == 0) return; --pos; }
    if (pos == 0 || pos >= textBuffer.size()) return;
    pushUndo();
    std::swap(textBuffer[pos - 1], textBuffer[pos]);
    selections_[0].anchor = selections_[0].cursor = pos + 1;
    invalidateLineIndex(); dirty_ = true;
}

// Apply fn to the selected lines, or to the whole document when nothing is selected.
template <class Fn>
static void transformLines(Application& app, std::string& text, std::vector<SelRange>& sels, Fn fn, size_t a, size_t b) {
    (void)app;
    std::vector<std::string> lines;
    std::string block = text.substr(a, b - a);
    size_t start = 0;
    while (true) {
        size_t nl = block.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(block.substr(start)); break; }
        lines.push_back(block.substr(start, nl - start));
        start = nl + 1;
    }
    bool trailing = lines.size() > 1 && lines.back().empty();
    if (trailing) lines.pop_back();
    fn(lines);
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) { if (i) joined += '\n'; joined += lines[i]; }
    if (trailing) joined += '\n';
    text.replace(a, b - a, joined);
    sels.clear(); sels.emplace_back(a, a + joined.size());
}

void Application::sortLines(bool caseSensitive) {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    size_t a = 0, b = textBuffer.size();
    if (selections_[0].hasSelection()) { a = lineStart(selections_[0].min()); b = lineEnd(selections_[0].max()); }
    transformLines(*this, textBuffer, selections_, [&](std::vector<std::string>& lines) {
        std::stable_sort(lines.begin(), lines.end(), [&](const std::string& x, const std::string& y) {
            return caseSensitive ? x < y : lowered(x) < lowered(y);
        });
    }, a, b);
    invalidateLineIndex(); dirty_ = true;
}

void Application::reverseLines() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    size_t a = 0, b = textBuffer.size();
    if (selections_[0].hasSelection()) { a = lineStart(selections_[0].min()); b = lineEnd(selections_[0].max()); }
    transformLines(*this, textBuffer, selections_, [](std::vector<std::string>& lines) { std::reverse(lines.begin(), lines.end()); }, a, b);
    invalidateLineIndex(); dirty_ = true;
}

void Application::uniqueLines() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    size_t a = 0, b = textBuffer.size();
    if (selections_[0].hasSelection()) { a = lineStart(selections_[0].min()); b = lineEnd(selections_[0].max()); }
    transformLines(*this, textBuffer, selections_, [](std::vector<std::string>& lines) {
        std::vector<std::string> kept;
        for (auto& line : lines) if (std::find(kept.begin(), kept.end(), line) == kept.end()) kept.push_back(line);
        lines = std::move(kept);
    }, a, b);
    invalidateLineIndex(); dirty_ = true;
}

void Application::addPreviousLine() {
    if (selections_.empty()) return;
    size_t line = lineOfPos(selections_.front().cursor);
    if (line == 0) return;
    size_t ls = lineStartForLine(line - 1);
    size_t col = std::min(selections_.front().cursor - lineStart(selections_.front().cursor), lineEnd(ls) - ls);
    selections_.insert(selections_.begin(), SelRange(ls + col));
}

void Application::addNextLine() {
    if (selections_.empty()) return;
    size_t line = lineOfPos(selections_.back().cursor);
    if (line + 1 >= totalLines()) return;
    size_t ls = lineStartForLine(line + 1);
    size_t col = std::min(selections_.back().cursor - lineStart(selections_.back().cursor), lineEnd(ls) - ls);
    selections_.emplace_back(ls + col);
}

void Application::newWindow() {
#ifdef _WIN32
    fs::path exe = fs::path(paths_.exeDir) / "moreno_text.exe";
    if (fs::exists(exe)) ShellExecuteA(nullptr, "open", exe.string().c_str(), nullptr, paths_.exeDir.c_str(), SW_SHOWNORMAL);
#endif
}

void Application::closeAllFiles() {
    for (int guard = 0; guard < 10000 && !closeConfirmOpen_ && !tabs_.empty(); ++guard) {
        saveCurrentTab();
        bool lone = tabs_.size() == 1 && tabs_[0].filePath.empty() && !tabs_[0].dirty && tabs_[0].fileName == "untitled";
        if (lone) break;
        closeTab(tabs_.size() - 1);
    }
}

void Application::closeFolder() {
    sidebarRoot_.clear(); sidebarTree_ = {}; sidebarVisible_ = false;
    saveSession();
}

void Application::refreshFolder() {
    if (!sidebarRoot_.empty()) loadFolder(sidebarRoot_);
}

void Application::openDocumentation() { openExternal("https://github.com/MorenoLand/Moreno.Text"); }
void Application::reportBug() { openExternal("https://github.com/MorenoLand/Moreno.Text/issues"); }

void Application::showAbout() {
    pluginStatusText_ = std::string("Moreno Text ") + MORENO_TEXT_VERSION;
    pluginStatusUntil_ = SDL_GetTicks() + 6000;
}

void Application::foldCurrent() {
    if (selections_.empty()) return;
    int line = static_cast<int>(lineOfPos(selections_[0].cursor));
    auto folded = std::find_if(foldedRegions_.begin(), foldedRegions_.end(), [line](const auto& r) { return r.first == line; });
    if (folded == foldedRegions_.end()) toggleFold(static_cast<size_t>(line));
}

void Application::unfoldCurrent() {
    if (selections_.empty()) return;
    int line = static_cast<int>(lineOfPos(selections_[0].cursor));
    for (auto it = foldedRegions_.begin(); it != foldedRegions_.end(); ++it) {
        if (it->first == line || (line > it->first && line <= it->second)) { foldedRegions_.erase(it); break; }
    }
}

void Application::convertLineEndings(bool crlf) {
    if (largeFileGuarded_ || binaryView_) return;
    pushUndo();
    std::string out;
    out.reserve(textBuffer.size());
    for (size_t i = 0; i < textBuffer.size(); ++i) {
        char c = textBuffer[i];
        if (c == '\r') {
            if (i + 1 < textBuffer.size() && textBuffer[i + 1] == '\n') ++i;
            out += crlf ? "\r\n" : "\n";
        } else if (c == '\n') {
            out += crlf ? "\r\n" : "\n";
        } else {
            out += c;
        }
    }
    if (out == textBuffer) return;
    size_t pos = selections_.empty() ? 0 : std::min(selections_[0].cursor, textBuffer.size());
    size_t newPos = 0;
    for (size_t i = 0; i < pos && i < textBuffer.size(); ++i) {
        if (textBuffer[i] == '\r' && i + 1 < textBuffer.size() && textBuffer[i + 1] == '\n') continue;
        newPos += (crlf && (textBuffer[i] == '\n' || textBuffer[i] == '\r')) ? 2 : 1;
    }
    textBuffer = std::move(out);
    selections_.clear(); selections_.emplace_back(std::min(newPos, textBuffer.size()));
    invalidateLineIndex(); dirty_ = true;
}

// Enter: keep the current line's indentation, indent after an opening bracket (or a trailing colon in
// Python) and open a blank line between a bracket pair.
void Application::insertNewline() {
    if (binaryView_ || largeFileGuarded_ || selections_.empty()) return;
    if (!autoIndent_) { insertText("\n"); return; }
    std::string lang = syntax_->languageName();
    for (auto& c : lang) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool colonOpens = lang == "python" || lang == "yaml";
    std::string unit = useTabs_ ? "\t" : std::string(static_cast<size_t>(tabSize_), ' ');
    std::vector<SelRange> sels = selections_;
    std::sort(sels.begin(), sels.end(), [](const SelRange& a, const SelRange& b) { return a.min() < b.min(); });
    pushUndo();
    std::vector<SelRange> updated;
    long shift = 0;
    for (const SelRange& sel : sels) {
        size_t a = static_cast<size_t>(static_cast<long>(sel.min()) + shift), b = static_cast<size_t>(static_cast<long>(sel.max()) + shift);
        size_t ls = lineStart(a);
        size_t indentEnd = ls;
        while (indentEnd < a && (textBuffer[indentEnd] == ' ' || textBuffer[indentEnd] == '\t')) ++indentEnd;
        std::string indent = textBuffer.substr(ls, indentEnd - ls);
        char prev = 0;
        for (size_t i = a; i-- > ls;) { if (textBuffer[i] != ' ' && textBuffer[i] != '\t') { prev = textBuffer[i]; break; } }
        char next = b < textBuffer.size() ? textBuffer[b] : 0;
        bool opener = prev == '{' || prev == '(' || prev == '[' || (colonOpens && prev == ':');
        bool pair = (prev == '{' && next == '}') || (prev == '(' && next == ')') || (prev == '[' && next == ']');
        std::string text;
        size_t cursorOffset;
        if (opener && pair) { text = "\n" + indent + unit + "\n" + indent; cursorOffset = 1 + indent.size() + unit.size(); }
        else { text = "\n" + indent + (opener ? unit : std::string()); cursorOffset = text.size(); }
        textBuffer.replace(a, b - a, text);
        shift += static_cast<long>(text.size()) - static_cast<long>(b - a) - (static_cast<long>(sel.max()) - static_cast<long>(sel.min())) + (static_cast<long>(sel.max()) - static_cast<long>(sel.min()));
        updated.emplace_back(a + cursorOffset);
    }
    selections_ = updated;
    invalidateLineIndex(); dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true; maxLineWidthDirty_ = true;
    desiredCursorX_ = -1.f;
    ensureCursorVisible();
}

// Tab key: indent every selected line, or insert spaces up to the next tab stop (or a tab character).
void Application::pressTab(bool shift) {
    if (binaryView_ || selections_.empty()) return;
    const SelRange& sel = selections_.front();
    if (shift) { unindentSelection(); return; }
    if (sel.hasSelection() && lineOfPos(sel.min()) != lineOfPos(sel.max())) { indentSelection(); return; }
    if (useTabs_) { insertText("\t"); return; }
    size_t column = sel.min() - lineStart(sel.min());
    size_t spaces = static_cast<size_t>(tabSize_) - column % static_cast<size_t>(tabSize_);
    insertText(std::string(spaces, ' '));
}
