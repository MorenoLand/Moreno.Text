// Tab switching, quick find, marks, paragraph wrapping and bracket navigation.
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "UI/StatusBar.h"
#include "UI/Titlebar.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#ifdef None
#undef None
#endif

namespace {

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

}  // namespace

void Application::nextTab() {
    if (tabs_.size() > 1) switchToTab((activeTab_ + 1) % tabs_.size());
}

void Application::previousTab() {
    if (tabs_.size() > 1) switchToTab((activeTab_ + tabs_.size() - 1) % tabs_.size());
}

void Application::selectTabByIndex(size_t index) {
    if (index < tabs_.size() && index != activeTab_) switchToTab(index);
}

void Application::selectLastTab() {
    if (!tabs_.empty()) selectTabByIndex(tabs_.size() - 1);
}

std::string Application::wordOrSelectionAtCursor() const {
    if (selections_.empty()) return {};
    const SelRange& sel = selections_.back();
    if (sel.hasSelection()) {
        if (sel.max() - sel.min() > 200) return {};
        return textBuffer.substr(sel.min(), sel.max() - sel.min());
    }
    size_t s = sel.cursor, e = sel.cursor;
    while (s > 0 && isWordChar(textBuffer[s - 1])) --s;
    while (e < textBuffer.size() && isWordChar(textBuffer[e])) ++e;
    return textBuffer.substr(s, e - s);
}

void Application::findUnder(bool forward) {
    std::string text = wordOrSelectionAtCursor();
    if (text.empty()) return;
    find_.query = text; find_.regex = false; find_.wholeWord = false;
    if (forward) findNext(); else findPrevious();
}

void Application::findAllUnder() {
    std::string text = wordOrSelectionAtCursor();
    if (!text.empty()) { find_.query = text; find_.regex = false; find_.wholeWord = false; }
    findAllSelect();
}

void Application::quickSkipNext() {
    if (selections_.empty()) return;
    SelRange last = selections_.back();
    if (!last.hasSelection()) { expandSelectionToWord(); return; }
    std::string word = textBuffer.substr(last.min(), last.max() - last.min());
    size_t found = textBuffer.find(word, last.max());
    if (found == std::string::npos) found = textBuffer.find(word);
    if (found == std::string::npos || found == last.min()) return;
    selections_.back() = SelRange(found, found + word.size());
    ensureCursorVisible();
}

void Application::useSelectionForFind() {
    std::string text = wordOrSelectionAtCursor();
    if (text.empty()) return;
    find_.query = text; find_.regex = false;
    findAllMatches();
}

void Application::useSelectionForReplace() {
    std::string text = wordOrSelectionAtCursor();
    if (!text.empty()) find_.replace = text;
}

void Application::replaceNext() {
    if (find_.query.empty() || selections_.empty()) return;
    findAllMatches();
    if (find_.matches.empty()) return;
    size_t from = selections_[0].min(), idx = 0;
    while (idx < find_.matches.size() && find_.matches[idx] < from) ++idx;
    if (idx == find_.matches.size()) idx = 0;
    find_.currentMatch = idx;
    doReplace();
    findNext();
}

void Application::setMark() {
    if (!selections_.empty()) markPos_ = selections_[0].cursor;
}

void Application::selectToMark() {
    if (selections_.empty() || markPos_ == std::string::npos) return;
    selections_[0].anchor = std::min(markPos_, textBuffer.size());
    ensureCursorVisible();
}

void Application::deleteToMark() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty() || markPos_ == std::string::npos) return;
    size_t a = std::min(markPos_, selections_[0].cursor), b = std::max(markPos_, selections_[0].cursor);
    b = std::min(b, textBuffer.size());
    if (a >= b) return;
    pushUndo();
    killRing_ = textBuffer.substr(a, b - a);
    textBuffer.erase(a, b - a);
    selections_.clear(); selections_.emplace_back(a);
    markPos_ = std::string::npos;
    invalidateLineIndex(); dirty_ = true;
}

void Application::swapWithMark() {
    if (selections_.empty() || markPos_ == std::string::npos) return;
    size_t cursor = selections_[0].cursor;
    selections_[0].anchor = selections_[0].cursor = std::min(markPos_, textBuffer.size());
    markPos_ = cursor;
    ensureCursorVisible();
}

void Application::yank() {
    if (killRing_.empty() || largeFileGuarded_ || binaryView_) return;
    insertText(killRing_);
}

void Application::showAtCenter() {
    if (selections_.empty() || wordWrap_) return;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_, &ww, &wh);
    float lineStep = fontAtlas().lineHeight();
    float viewH = static_cast<float>(wh) - titlebar_->height() - tabBarH_ - statusbar_->height();
    scrollY_ = std::max(0.f, lineOfPos(selections_[0].cursor) * lineStep - viewH * 0.5f + lineStep * 0.5f);
}

void Application::wrapParagraph(size_t width) {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    size_t a, b;
    if (selections_[0].hasSelection()) { a = lineStart(selections_[0].min()); b = lineEnd(selections_[0].max()); }
    else {
        auto blank = [&](size_t line) {
            size_t s = lineStartForLine(line), e = lineEnd(s);
            for (size_t i = s; i < e; ++i) if (!std::isspace(static_cast<unsigned char>(textBuffer[i]))) return false;
            return true;
        };
        size_t line = lineOfPos(selections_[0].cursor);
        if (blank(line)) return;
        size_t lo = line, hi = line;
        while (lo > 0 && !blank(lo - 1)) --lo;
        while (hi + 1 < totalLines() && !blank(hi + 1)) ++hi;
        a = lineStartForLine(lo); b = lineEnd(lineStartForLine(hi));
    }
    if (a >= b) return;
    std::string block = textBuffer.substr(a, b - a);
    size_t firstEnd = block.find('\n');
    std::string first = block.substr(0, firstEnd);
    size_t indentLen = 0;
    while (indentLen < first.size() && (first[indentLen] == ' ' || first[indentLen] == '\t')) ++indentLen;
    std::string prefix = first.substr(0, indentLen);
    static const char* commentTokens[] = {"//", "#", "--", ";", "*"};
    for (auto tok : commentTokens) {
        size_t n = std::strlen(tok);
        if (first.compare(indentLen, n, tok) == 0) {
            size_t after = indentLen + n;
            while (after < first.size() && first[after] == ' ') ++after;
            prefix = first.substr(0, after);
            break;
        }
    }
    std::vector<std::string> words;
    std::istringstream in(block);
    for (std::string line; std::getline(in, line);) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        for (auto tok : commentTokens) {
            size_t n = std::strlen(tok);
            if (line.compare(i, n, tok) == 0) { i += n; break; }
        }
        std::istringstream ws(line.substr(std::min(i, line.size())));
        for (std::string w; ws >> w;) words.push_back(w);
    }
    std::string out, current = prefix;
    bool lineHasWord = false;
    for (auto& w : words) {
        if (lineHasWord && current.size() + 1 + w.size() > width) { out += current + "\n"; current = prefix; lineHasWord = false; }
        if (lineHasWord) current += ' ';
        current += w; lineHasWord = true;
    }
    out += current;
    pushUndo();
    textBuffer.replace(a, b - a, out);
    selections_.clear(); selections_.emplace_back(a + out.size());
    invalidateLineIndex(); dirty_ = true;
}

void Application::jumpToMatchingBracket() {
    if (selections_.empty()) return;
    size_t pos = selections_[0].cursor;
    auto isOpen = [](char c) { return c == '(' || c == '[' || c == '{'; };
    auto isClose = [](char c) { return c == ')' || c == ']' || c == '}'; };
    size_t at = textBuffer.size();
    if (pos < textBuffer.size() && (isOpen(textBuffer[pos]) || isClose(textBuffer[pos]))) at = pos;
    else if (pos > 0 && (isOpen(textBuffer[pos - 1]) || isClose(textBuffer[pos - 1]))) at = pos - 1;
    if (at == textBuffer.size()) return;
    char c = textBuffer[at];
    char pair = c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == ')' ? '(' : c == ']' ? '[' : '{';
    int depth = 0;
    if (isOpen(c)) {
        for (size_t i = at; i < textBuffer.size(); ++i) {
            if (textBuffer[i] == c) ++depth;
            else if (textBuffer[i] == pair && --depth == 0) { selections_.clear(); selections_.emplace_back(i); ensureCursorVisible(); return; }
        }
    } else {
        for (size_t i = at + 1; i-- > 0;) {
            if (textBuffer[i] == c) ++depth;
            else if (textBuffer[i] == pair && --depth == 0) { selections_.clear(); selections_.emplace_back(i); ensureCursorVisible(); return; }
        }
    }
}

void Application::foldByLevel(int level) {
    foldAll();
    std::vector<std::pair<int, int>> all(foldedRegions_.begin(), foldedRegions_.end());
    foldedRegions_.clear();
    for (auto& r : all) {
        int depth = 1;
        for (auto& o : all) if (o.first < r.first && o.second >= r.second) ++depth;
        if (depth == level) foldedRegions_.insert(r);
    }
}

void Application::showCompletions() {
    if (selections_.empty() || binaryView_) return;
    size_t pos = selections_[0].cursor, s = pos;
    while (s > 0 && isWordChar(textBuffer[s - 1])) --s;
    acPrefix_ = textBuffer.substr(s, pos - s);
    acPrefixStart_ = s;
    updateAutocomplete();
}

void Application::setLayoutColumns(size_t columns) {
    saveCurrentTab();
    size_t base = activeTab_;
    if (columns <= 1) { collapseEditorGroupsTo(base); loadTab(base); return; }
    columns = std::min<size_t>(columns, 8);
    while (tabs_.size() < columns) newBuffer();
    saveCurrentTab();
    collapseEditorGroupsTo(base);
    for (size_t i = 0; i < tabs_.size() && editorGroups_.size() < columns; ++i) {
        if (i == base) continue;
        addEditorGroupForTab(i);
    }
    activeGroup_ = 0;
    loadTab(base);
}

void Application::focusGroup(size_t index) {
    normalizeEditorGroups();
    if (index >= editorGroups_.size() || index == activeGroup_) return;
    saveCurrentTab();
    activeGroup_ = index;
    loadTab(editorGroups_[index].tab);
}

void Application::focusNeighboringGroup(bool next) {
    normalizeEditorGroups();
    if (editorGroups_.size() < 2) return;
    focusGroup(next ? (activeGroup_ + 1) % editorGroups_.size() : (activeGroup_ + editorGroups_.size() - 1) % editorGroups_.size());
}

void Application::moveFileToGroup(size_t index) {
    normalizeEditorGroups();
    if (index >= editorGroups_.size() || index == activeGroup_) return;
    saveCurrentTab();
    std::swap(editorGroups_[activeGroup_].tab, editorGroups_[index].tab);
    activeGroup_ = index;
    loadTab(editorGroups_[index].tab);
}

void Application::moveFileToNeighboringGroup(bool next) {
    normalizeEditorGroups();
    if (editorGroups_.size() < 2) return;
    moveFileToGroup(next ? (activeGroup_ + 1) % editorGroups_.size() : (activeGroup_ + editorGroups_.size() - 1) % editorGroups_.size());
}

void Application::newGroup() {
    saveCurrentTab();
    normalizeEditorGroups();
    newBuffer();
    addEditorGroupForTab(activeTab_);
}

void Application::closeGroup() {
    normalizeEditorGroups();
    if (editorGroups_.size() < 2) return;
    saveCurrentTab();
    editorGroups_.erase(editorGroups_.begin() + static_cast<std::ptrdiff_t>(activeGroup_));
    if (activeGroup_ >= editorGroups_.size()) activeGroup_ = editorGroups_.size() - 1;
    loadTab(editorGroups_[activeGroup_].tab);
}
