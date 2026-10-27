// Menu and key commands that act on the current selection, search results and view layout.
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Syntax/SyntaxHighlighter.h"
#include "UI/StatusBar.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

bool isBlank(const std::string& s, size_t a, size_t b) {
    for (size_t i = a; i < b; ++i) if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r') return false;
    return true;
}

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

void Application::openGotoWithPrefix(const std::string& prefix) {
    recordJump();
    goto_.active = true; goto_.query = prefix; goto_.selected = 0; goto_.items.clear();
    updateGotoResults();
}

void Application::recordJump() {
    if (selections_.empty()) return;
    std::pair<size_t, size_t> here{activeTab_, selections_[0].cursor};
    if (jumpIndex_ < jumpStack_.size()) jumpStack_.resize(jumpIndex_ + 1);
    if (!jumpStack_.empty() && jumpStack_.back() == here) { jumpIndex_ = jumpStack_.size(); return; }
    jumpStack_.push_back(here);
    if (jumpStack_.size() > 100) jumpStack_.erase(jumpStack_.begin());
    jumpIndex_ = jumpStack_.size();
}

void Application::applyJump(size_t index) {
    if (index >= jumpStack_.size()) return;
    auto target = jumpStack_[index];
    if (target.first != activeTab_ && target.first < tabs_.size()) switchToTab(target.first);
    size_t pos = std::min(target.second, textBuffer.size());
    selections_.clear(); selections_.emplace_back(pos);
    ensureCursorVisible();
}

void Application::jumpBack() {
    if (selections_.empty()) return;
    if (jumpIndex_ >= jumpStack_.size()) {
        std::pair<size_t, size_t> here{activeTab_, selections_[0].cursor};
        if (jumpStack_.empty() || jumpStack_.back() != here) jumpStack_.push_back(here);
        jumpIndex_ = jumpStack_.size() - 1;
    }
    if (jumpIndex_ == 0) return;
    applyJump(--jumpIndex_);
}

void Application::jumpForward() {
    if (jumpIndex_ + 1 >= jumpStack_.size()) return;
    applyJump(++jumpIndex_);
}

void Application::indentSelection() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    auto& sel = selections_[0];
    size_t a = sel.min(), b = sel.max();
    size_t first = lineOfPos(a), last = lineOfPos(b);
    if (b > a && b == lineStart(b) && last > first) --last;
    std::string unit = useTabs_ ? "\t" : std::string(static_cast<size_t>(tabSize_), ' ');
    std::vector<size_t> starts;
    for (size_t l = first; l <= last; ++l) starts.push_back(lineStartForLine(l));
    for (size_t i = starts.size(); i-- > 0;) textBuffer.insert(starts[i], unit);
    auto shift = [&](size_t p) {
        size_t n = 0;
        for (size_t s : starts) if (s <= p) ++n;
        return p + n * unit.size();
    };
    sel.anchor = shift(sel.anchor); sel.cursor = shift(sel.cursor);
    invalidateLineIndex(); dirty_ = true;
}

void Application::unindentSelection() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    pushUndo();
    auto& sel = selections_[0];
    size_t a = sel.min(), b = sel.max();
    size_t first = lineOfPos(a), last = lineOfPos(b);
    if (b > a && b == lineStart(b) && last > first) --last;
    std::vector<std::pair<size_t, size_t>> removed;  // line start, bytes removed
    for (size_t l = last + 1; l-- > first;) {
        size_t s = lineStartForLine(l), r = 0;
        if (s < textBuffer.size() && textBuffer[s] == '\t') r = 1;
        else while (r < static_cast<size_t>(tabSize_) && s + r < textBuffer.size() && textBuffer[s + r] == ' ') ++r;
        if (r) textBuffer.erase(s, r);
        removed.emplace_back(s, r);
    }
    auto shift = [&](size_t p) {
        size_t cut = 0;
        for (auto& [s, r] : removed) if (p > s) cut += std::min(r, p - s);
        return p - cut;
    };
    sel.anchor = shift(sel.anchor); sel.cursor = shift(sel.cursor);
    invalidateLineIndex(); dirty_ = true;
}

size_t Application::matchLengthAt(size_t pos) const {
    if (!find_.regex) return find_.query.size();
    try {
        std::regex re(find_.query, find_.caseSensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase));
        std::smatch m;
        auto begin = textBuffer.begin() + static_cast<std::ptrdiff_t>(pos);
        if (std::regex_search(begin, textBuffer.end(), m, re, std::regex_constants::match_continuous)) return static_cast<size_t>(m.length());
    } catch (...) {}
    return 0;
}

void Application::selectMatch(size_t pos) {
    size_t len = matchLengthAt(pos);
    selections_.clear(); selections_.emplace_back(pos, pos + len);
    ensureCursorVisible();
}

void Application::findNext() {
    if (find_.query.empty()) { commandFind(); return; }
    recordJump();
    findAllMatches();
    if (find_.matches.empty() || selections_.empty()) return;
    auto& sel = selections_[0];
    size_t from = sel.hasSelection() ? sel.max() : sel.cursor + 1;
    size_t idx = 0;
    while (idx < find_.matches.size() && find_.matches[idx] < from) ++idx;
    if (idx == find_.matches.size()) idx = 0;
    find_.currentMatch = idx;
    selectMatch(find_.matches[idx]);
}

void Application::findPrevious() {
    if (find_.query.empty()) { commandFind(); return; }
    recordJump();
    findAllMatches();
    if (find_.matches.empty() || selections_.empty()) return;
    auto& sel = selections_[0];
    size_t before = sel.min();
    size_t idx = find_.matches.size();
    while (idx > 0 && find_.matches[idx - 1] >= before) --idx;
    idx = idx == 0 ? find_.matches.size() - 1 : idx - 1;
    find_.currentMatch = idx;
    selectMatch(find_.matches[idx]);
}

void Application::findAllSelect() {
    if (find_.query.empty()) { commandFind(); return; }
    recordJump();
    findAllMatches();
    if (find_.matches.empty()) return;
    std::vector<SelRange> all;
    for (size_t pos : find_.matches) all.emplace_back(pos, pos + matchLengthAt(pos));
    selections_ = std::move(all);
    ensureCursorVisible();
}

void Application::expandSelectionToLine() {
    for (auto& sel : selections_) {
        size_t a = lineStart(sel.min()), b = sel.max();
        bool wholeLines = sel.hasSelection() && sel.min() == a && (b == textBuffer.size() || textBuffer[b - 1] == '\n');
        size_t e = lineEnd(b);
        if (wholeLines && b >= textBuffer.size()) continue;
        sel.anchor = a;
        sel.cursor = e < textBuffer.size() ? e + 1 : e;
    }
    ensureCursorVisible();
}

void Application::expandSelectionToWord() {
    if (selections_.empty()) return;
    auto& sel = selections_.back();
    if (!sel.hasSelection()) {
        size_t s = sel.cursor, e = sel.cursor;
        while (s > 0 && isWordChar(textBuffer[s - 1])) --s;
        while (e < textBuffer.size() && isWordChar(textBuffer[e])) ++e;
        if (e > s) { sel.anchor = s; sel.cursor = e; }
        return;
    }
    std::string word = textBuffer.substr(sel.min(), sel.max() - sel.min());
    size_t found = textBuffer.find(word, sel.max());
    if (found == std::string::npos) found = textBuffer.find(word);
    if (found == std::string::npos) return;
    for (auto& s : selections_) if (s.min() == found) return;
    selections_.emplace_back(found, found + word.size());
    ensureCursorVisible();
}

void Application::expandSelectionToBrackets() {
    if (selections_.empty()) return;
    auto& sel = selections_[0];
    size_t a = sel.min(), b = sel.max();
    auto closerOf = [](char c) { return c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : '\0'; };
    for (size_t start = a; start-- > 0;) {
        char open = textBuffer[start];
        char close = closerOf(open);
        if (!close) continue;
        int depth = 0; size_t end = start;
        for (; end < textBuffer.size(); ++end) {
            if (textBuffer[end] == open) ++depth;
            else if (textBuffer[end] == close && --depth == 0) break;
        }
        if (end >= textBuffer.size() || end < b) continue;
        size_t innerA = start + 1, innerB = end;
        if (a == innerA && b == innerB) { sel.anchor = start; sel.cursor = end + 1; }
        else if (a > innerA || b < innerB) { sel.anchor = innerA; sel.cursor = innerB; }
        else continue;
        ensureCursorVisible();
        return;
    }
}

void Application::expandSelectionToIndentation() {
    if (selections_.empty()) return;
    auto& sel = selections_[0];
    auto indentOf = [&](size_t line) {
        size_t s = lineStartForLine(line), e = lineEnd(s), w = 0;
        if (isBlank(textBuffer, s, e)) return static_cast<size_t>(-1);
        while (s + w < e && (textBuffer[s + w] == ' ' || textBuffer[s + w] == '\t')) ++w;
        return w;
    };
    size_t first = lineOfPos(sel.min()), last = lineOfPos(sel.max());
    size_t level = indentOf(first);
    for (size_t l = first; l <= last; ++l) { size_t w = indentOf(l); if (w != static_cast<size_t>(-1) && (level == static_cast<size_t>(-1) || w < level)) level = w; }
    if (level == static_cast<size_t>(-1)) return;
    size_t lo = first, hi = last, total = totalLines();
    while (lo > 0) { size_t w = indentOf(lo - 1); if (w != static_cast<size_t>(-1) && w < level) break; --lo; }
    while (hi + 1 < total) { size_t w = indentOf(hi + 1); if (w != static_cast<size_t>(-1) && w < level) break; ++hi; }
    while (hi > lo && indentOf(hi) == static_cast<size_t>(-1)) --hi;
    size_t a = lineStartForLine(lo), b = lineEnd(lineStartForLine(hi));
    if (a == sel.min() && b == sel.max()) return;
    sel.anchor = a; sel.cursor = b;
    ensureCursorVisible();
}

// There is no scope data to consult, so grow outward through the structures we can see:
// word, then the enclosing brackets, then the line, then its indentation block, then the file.
void Application::expandSelectionToScope() {
    if (selections_.empty()) return;
    auto before = std::make_pair(selections_[0].anchor, selections_[0].cursor);
    auto changed = [&] { return before != std::make_pair(selections_[0].anchor, selections_[0].cursor); };
    if (!selections_[0].hasSelection()) { expandSelectionToWord(); if (changed()) return; }
    expandSelectionToBrackets(); if (changed()) return;
    size_t a = selections_[0].min(), b = selections_[0].max();
    size_t ls = lineStart(a), le = lineEnd(b);
    if (ls < a || le > b) { selections_[0].anchor = ls; selections_[0].cursor = le; ensureCursorVisible(); return; }
    expandSelectionToIndentation(); if (changed()) return;
    selections_[0].anchor = 0; selections_[0].cursor = textBuffer.size();
}

void Application::splitSelectionIntoLines() {
    if (selections_.empty() || !selections_[0].hasSelection()) return;
    size_t a = selections_[0].min(), b = selections_[0].max();
    std::vector<SelRange> lines;
    size_t pos = a;
    while (pos <= b) {
        size_t le = lineEnd(pos);
        if (le > b) le = b;
        lines.emplace_back(pos, le);
        pos = le + 1;
        if (pos > b || pos == 0) break;
    }
    selections_ = std::move(lines);
}

void Application::singleSelection() {
    if (selections_.size() > 1) selections_.resize(1);
}

void Application::toggleTabs() {
    tabsVisible_ = !tabsVisible_;
    tabBarH_ = tabsVisible_ ? 35.f : 0.f;
    tabDropdownOpen_ = false;
}

void Application::toggleStatusBar() {
    statusBarVisible_ = !statusBarVisible_;
    statusbar_->setVisible(statusBarVisible_);
    statusPopup_ = StatusPopup::None;
}

void Application::toggleDistractionFree() {
    if (!distractionFree_) {
        dfSavedSidebar_ = sidebarVisible_; dfSavedMinimap_ = minimapVisible_;
        dfSavedTabs_ = tabsVisible_; dfSavedStatus_ = statusBarVisible_; dfSavedFullscreen_ = fullscreen_;
        sidebarVisible_ = false; minimapVisible_ = false;
        if (tabsVisible_) toggleTabs();
        if (statusBarVisible_) toggleStatusBar();
        if (!fullscreen_) toggleFullscreen();
        distractionFree_ = true;
    } else {
        sidebarVisible_ = dfSavedSidebar_; minimapVisible_ = dfSavedMinimap_;
        if (tabsVisible_ != dfSavedTabs_) toggleTabs();
        if (statusBarVisible_ != dfSavedStatus_) toggleStatusBar();
        if (fullscreen_ != dfSavedFullscreen_) toggleFullscreen();
        distractionFree_ = false;
    }
}

void Application::openSyntaxPicker() {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_, &ww, &wh);
    float synW = fontAtlas().measureText(syntax_->languageName());
    int rows = std::min(syntaxLangCount, 18);
    statusPopup_ = StatusPopup::Syntax;
    popupX_ = static_cast<float>(ww) - synW - 12.f;
    popupY_ = static_cast<float>(wh) - statusbar_->height() - rows * 24.f - 4.f;
    popupSelected_ = 0; popupScroll_ = 0;
}

void Application::openIndentationPicker() {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_, &ww, &wh);
    std::string label = useTabs_ ? ("Tab Size: " + std::to_string(tabSize_)) : ("Spaces: " + std::to_string(tabSize_));
    float indentW = fontAtlas().measureText(label), synW = fontAtlas().measureText(syntax_->languageName());
    statusPopup_ = StatusPopup::Indent;
    popupX_ = static_cast<float>(ww) - indentW - synW - 32.f - 12.f;
    popupY_ = static_cast<float>(wh) - statusbar_->height() - 8 * 24.f - 4.f;
    popupSelected_ = 0; popupScroll_ = 0;
}

void Application::commandFindInFiles() {
    find_.active = true; find_.replaceActive = false; find_.matches.clear();
    if (selections_.size() && selections_[0].hasSelection() && selections_[0].max() - selections_[0].min() < 200)
        find_.query = textBuffer.substr(selections_[0].min(), selections_[0].max() - selections_[0].min());
    findFocus_ = 0;
    findPanelIsInFiles_ = true;
    pluginStatusText_ = sidebarRoot_.empty() ? "Find in Files: open a folder first" : "Find in Files: type a query and press Enter";
    pluginStatusUntil_ = SDL_GetTicks() + 5000;
}

void Application::runFindInFiles() {
    findPanelIsInFiles_ = false;
    find_.active = false;
    if (find_.query.empty() || sidebarRoot_.empty()) return;
    std::regex re;
    bool useRegex = find_.regex;
    if (useRegex) {
        try { re = std::regex(find_.query, find_.caseSensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase)); }
        catch (...) { pluginStatusText_ = "Find in Files: invalid regular expression"; pluginStatusUntil_ = SDL_GetTicks() + 4000; return; }
    }
    std::string needle = find_.caseSensitive ? find_.query : lowered(find_.query);
    auto lineMatches = [&](const std::string& line) {
        if (useRegex) return std::regex_search(line, re);
        std::string hay = find_.caseSensitive ? line : lowered(line);
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) {
            if (!find_.wholeWord) return true;
            bool left = pos == 0 || !isWordChar(hay[pos - 1]);
            bool right = pos + needle.size() >= hay.size() || !isWordChar(hay[pos + needle.size()]);
            if (left && right) return true;
            pos += needle.size();
        }
        return false;
    };
    static const char* skipDirs[] = {".git", ".svn", ".hg", "node_modules", "build", ".vs"};
    std::ostringstream body;
    size_t files = 0, hitFiles = 0, hits = 0;
    const size_t maxHits = 5000;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(sidebarRoot_, fs::directory_options::skip_permission_denied, ec), end; it != end && hits < maxHits; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            std::string name = it->path().filename().string();
            for (auto skip : skipDirs) if (name == skip) { it.disable_recursion_pending(); break; }
            continue;
        }
        if (!it->is_regular_file(ec) || it->file_size(ec) > 4u * 1024u * 1024u) continue;
        std::ifstream in(it->path(), std::ios::binary);
        if (!in) continue;
        char head[4096];
        in.read(head, sizeof(head));
        if (std::memchr(head, 0, static_cast<size_t>(in.gcount()))) continue;
        in.clear(); in.seekg(0);
        ++files;
        std::string line, fileBlock;
        size_t lineNo = 0, fileHits = 0;
        while (std::getline(in, line) && hits + fileHits < maxHits) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (lineMatches(line)) {
                fileBlock += "  " + std::to_string(lineNo) + ": " + (line.size() > 300 ? line.substr(0, 300) + "..." : line) + "\n";
                ++fileHits;
            }
        }
        if (fileHits) {
            body << fs::relative(it->path(), sidebarRoot_, ec).generic_string() << ":\n" << fileBlock << "\n";
            ++hitFiles; hits += fileHits;
        }
    }
    std::ostringstream out;
    out << "Searching " << files << " files for \"" << find_.query << "\"\n\n" << body.str();
    out << hits << " match" << (hits == 1 ? "" : "es") << " across " << hitFiles << " file" << (hitFiles == 1 ? "" : "s");
    if (hits >= maxHits) out << " (stopped at " << maxHits << ")";
    out << "\n";
    newBuffer();
    textBuffer = out.str();
    invalidateLineIndex();
    openFile_ = "Find Results";
    if (activeTab_ < tabs_.size()) tabs_[activeTab_].fileName = "Find Results";
    dirty_ = false;
    selections_.clear(); selections_.emplace_back(0);
    indexFindResults();
}

void Application::indexFindResults() {
    findHits_.clear(); findResultLines_.clear(); findHitCursor_ = 0;
    std::string currentFile;
    size_t lineNo = 0, pos = 0;
    while (pos <= textBuffer.size()) {
        size_t end = textBuffer.find('\n', pos);
        if (end == std::string::npos) end = textBuffer.size();
        std::string line = textBuffer.substr(pos, end - pos);
        if (line.size() > 1 && line.back() == ':' && line[0] != ' ') currentFile = line.substr(0, line.size() - 1);
        else if (!currentFile.empty() && line.size() > 3 && line[0] == ' ' && line[1] == ' ' && std::isdigit(static_cast<unsigned char>(line[2]))) {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                size_t target = static_cast<size_t>(std::strtoull(line.c_str() + 2, nullptr, 10));
                findResultLines_.emplace_back(lineNo, findHits_.size());
                findHits_.emplace_back((fs::path(sidebarRoot_) / currentFile).string(), target);
            }
        }
        ++lineNo;
        if (end >= textBuffer.size()) break;
        pos = end + 1;
    }
}

void Application::openFindHit(size_t index) {
    if (index >= findHits_.size()) return;
    findHitCursor_ = index;
    openFile(findHits_[index].first);
    size_t line = findHits_[index].second;
    if (line > 0) {
        selections_.clear(); selections_.emplace_back(lineStartForLine(line - 1));
        ensureCursorVisible();
    }
}

void Application::nextResult() {
    if (findHits_.empty()) return;
    openFindHit((findHitCursor_ + 1) % findHits_.size());
}

void Application::previousResult() {
    if (findHits_.empty()) return;
    openFindHit((findHitCursor_ + findHits_.size() - 1) % findHits_.size());
}

bool Application::openFindResultAtCursor() {
    if ((openFile_ != "Find Results" && openFile_ != "Build Results") || selections_.empty()) return false;
    size_t line = lineOfPos(selections_[0].cursor);
    for (auto& entry : findResultLines_) {
        if (entry.first == line) { openFindHit(entry.second); return true; }
    }
    return false;
}
