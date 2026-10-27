// Identifier case conversion, tag closing, reindent, permutations, selection history, paste history, rulers.
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <random>
#include <set>

#ifdef None
#undef None
#endif

namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; }

std::vector<std::string> splitIdentifier(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    auto flush = [&] { if (!current.empty()) { words.push_back(current); current.clear(); } };
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isalnum(c)) { flush(); continue; }
        if (!current.empty()) {
            unsigned char prev = static_cast<unsigned char>(current.back());
            bool boundary = (std::islower(prev) && std::isupper(c)) || (std::isdigit(prev) != std::isdigit(c) && false) ||
                            (std::isupper(prev) && std::isupper(c) && i + 1 < text.size() && std::islower(static_cast<unsigned char>(text[i + 1])));
            if (boundary) flush();
        }
        current += static_cast<char>(c);
    }
    flush();
    return words;
}

std::string capitalized(std::string w) {
    w = lowered(w);
    if (!w.empty()) w[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(w[0])));
    return w;
}

}  // namespace

void Application::setRuler(int column) {
    rulers_.clear();
    if (column > 0) rulers_.push_back(column);
    auto& settings = SettingsManager::instance();
    settings.set("rulers", rulers_);
    settings.save();
}

void Application::setWrapColumn(int column) {
    wrapColumn_ = column;
    if (column > 0) wordWrap_ = true;
    visualLines_.clear();
    auto& settings = SettingsManager::instance();
    settings.set("wrap_width", column);
    settings.save();
}

void Application::trackSelectionHistory() {
    if (selections_.empty()) return;
    bool significant = selections_.size() != selectionSnapshot_.size();
    if (!significant && selections_.size() == 1 && selections_[0].hasSelection() != selectionSnapshot_[0].hasSelection()) significant = true;
    if (significant && !selectionSnapshot_.empty()) {
        selectionHistory_.push_back(selectionSnapshot_);
        if (selectionHistory_.size() > 100) selectionHistory_.erase(selectionHistory_.begin());
        selectionRedo_.clear();
    }
    selectionSnapshot_ = selections_;
}

void Application::softUndo() {
    if (selectionHistory_.empty()) return;
    selectionRedo_.push_back(selections_);
    selections_ = selectionHistory_.back();
    selectionHistory_.pop_back();
    for (auto& s : selections_) { s.anchor = std::min(s.anchor, textBuffer.size()); s.cursor = std::min(s.cursor, textBuffer.size()); }
    selectionSnapshot_ = selections_;
    ensureCursorVisible();
}

void Application::softRedo() {
    if (selectionRedo_.empty()) return;
    selectionHistory_.push_back(selections_);
    selections_ = selectionRedo_.back();
    selectionRedo_.pop_back();
    for (auto& s : selections_) { s.anchor = std::min(s.anchor, textBuffer.size()); s.cursor = std::min(s.cursor, textBuffer.size()); }
    selectionSnapshot_ = selections_;
    ensureCursorVisible();
}

void Application::rememberClipboardText(const std::string& text) {
    clipHistory_.erase(std::remove(clipHistory_.begin(), clipHistory_.end(), text), clipHistory_.end());
    clipHistory_.insert(clipHistory_.begin(), text);
    if (clipHistory_.size() > 30) clipHistory_.pop_back();
}

void Application::updatePasteHistoryResults(const std::string& pattern) {
    std::string needle = lowered(pattern);
    for (size_t i = 0; i < clipHistory_.size(); ++i) {
        if (!needle.empty() && lowered(clipHistory_[i]).find(needle) == std::string::npos) continue;
        std::string first = clipHistory_[i].substr(0, clipHistory_[i].find('\n'));
        if (first.size() > 90) first = first.substr(0, 90) + "...";
        goto_.items.push_back(first);
        goto_.subtexts.push_back(std::to_string(clipHistory_[i].size()) + " chars");
        goto_.data.push_back(std::to_string(i));
    }
}

void Application::openPasteHistory() {
    if (clipHistory_.empty()) {
        char* clip = SDL_GetClipboardText();
        if (clip && *clip) rememberClipboardText(clip);
        if (clip) SDL_free(clip);
    }
    openGotoWithPrefix("%");
}

// style: 0 lowerCamelCase, 1 UpperCamelCase, 2 snake_case, 3 kebab-case
void Application::convertIdentifierCase(int style) {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (auto& sel : selections_) {
        size_t a = sel.min(), b = sel.max();
        if (a == b) {
            while (a > 0 && isIdentChar(textBuffer[a - 1])) --a;
            while (b < textBuffer.size() && isIdentChar(textBuffer[b])) ++b;
        }
        if (b > a) ranges.emplace_back(a, b);
    }
    if (ranges.empty()) return;
    std::sort(ranges.begin(), ranges.end());
    pushUndo();
    std::vector<SelRange> updated;
    long shift = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        size_t a = ranges[i].first, b = ranges[i].second;
        if (i && a < ranges[i - 1].second) continue;
        auto words = splitIdentifier(textBuffer.substr(static_cast<size_t>(static_cast<long>(a) + shift), b - a));
        std::string out;
        for (size_t w = 0; w < words.size(); ++w) {
            if (style == 0) out += w == 0 ? lowered(words[w]) : capitalized(words[w]);
            else if (style == 1) out += capitalized(words[w]);
            else out += (w ? (style == 2 ? "_" : "-") : "") + lowered(words[w]);
        }
        if (out.empty()) continue;
        size_t at = static_cast<size_t>(static_cast<long>(a) + shift);
        textBuffer.replace(at, b - a, out);
        updated.emplace_back(at, at + out.size());
        shift += static_cast<long>(out.size()) - static_cast<long>(b - a);
    }
    if (!updated.empty()) selections_ = updated;
    invalidateLineIndex(); dirty_ = true;
}

void Application::closeTag() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    static const std::set<std::string> voidTags = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    size_t end = selections_[0].cursor;
    std::vector<std::string> open;
    for (size_t i = 0; i < end;) {
        if (textBuffer[i] != '<') { ++i; continue; }
        if (textBuffer.compare(i, 4, "<!--") == 0) { size_t close = textBuffer.find("-->", i + 4); i = close == std::string::npos || close + 3 > end ? end : close + 3; continue; }
        size_t j = i + 1;
        bool closing = j < end && textBuffer[j] == '/';
        if (closing) ++j;
        size_t nameStart = j;
        while (j < end && (std::isalnum(static_cast<unsigned char>(textBuffer[j])) || textBuffer[j] == '-' || textBuffer[j] == ':' || textBuffer[j] == '_')) ++j;
        std::string name = lowered(textBuffer.substr(nameStart, j - nameStart));
        size_t gt = textBuffer.find('>', j);
        if (name.empty() || gt == std::string::npos || gt >= end) { i = j > i ? j : i + 1; continue; }
        bool selfClosing = textBuffer[gt - 1] == '/';
        if (closing) { auto it = std::find(open.rbegin(), open.rend(), name); if (it != open.rend()) open.erase(std::next(it).base(), open.end()); }
        else if (!selfClosing && !voidTags.count(name)) open.push_back(name);
        i = gt + 1;
    }
    if (open.empty()) return;
    insertText("</" + open.back() + ">");
}

void Application::reindent() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    size_t a = 0, b = textBuffer.size();
    if (selections_[0].hasSelection()) { a = lineStart(selections_[0].min()); b = lineEnd(selections_[0].max()); }
    size_t firstLine = lineOfPos(a), lastLine = lineOfPos(b);
    std::string unit = useTabs_ ? "\t" : std::string(static_cast<size_t>(tabSize_), ' ');
    int depth = 0;
    bool inBlockComment = false;
    // Depth at the start of the region comes from every line before it.
    auto scan = [&](const std::string& line, int& d, int& leadingClosers) {
        leadingClosers = 0;
        bool leading = true, inStr = false;
        char quote = 0;
        for (size_t k = 0; k < line.size(); ++k) {
            char c = line[k];
            if (inBlockComment) { if (c == '*' && k + 1 < line.size() && line[k + 1] == '/') { inBlockComment = false; ++k; } continue; }
            if (inStr) { if (c == '\\') ++k; else if (c == quote) inStr = false; continue; }
            if (c == '/' && k + 1 < line.size() && line[k + 1] == '/') break;
            if (c == '#' && k == line.find_first_not_of(" \t") && false) break;
            if (c == '/' && k + 1 < line.size() && line[k + 1] == '*') { inBlockComment = true; ++k; continue; }
            if (c == '"' || c == '\'') { inStr = true; quote = c; leading = false; continue; }
            if (c == ' ' || c == '\t') continue;
            if (c == '{' || c == '(' || c == '[') { ++d; leading = false; }
            else if (c == '}' || c == ')' || c == ']') { --d; if (leading) ++leadingClosers; }
            else leading = false;
        }
    };
    std::vector<std::string> lines;
    size_t total = totalLines();
    for (size_t l = 0; l < total; ++l) { size_t s = lineStartForLine(l); lines.push_back(textBuffer.substr(s, lineEnd(s) - s)); }
    int dummy = 0;
    for (size_t l = 0; l < firstLine && l < lines.size(); ++l) scan(lines[l], depth, dummy);
    pushUndo();
    std::string result;
    for (size_t l = firstLine; l <= lastLine && l < lines.size(); ++l) {
        std::string trimmed = lines[l];
        size_t nonSpace = trimmed.find_first_not_of(" \t");
        trimmed = nonSpace == std::string::npos ? std::string() : trimmed.substr(nonSpace);
        int before = depth, leadingClosers = 0;
        scan(trimmed, depth, leadingClosers);
        int level = std::max(0, before - leadingClosers);
        std::string indented;
        if (!trimmed.empty()) for (int k = 0; k < level; ++k) indented += unit;
        if (l > firstLine) result += '\n';
        result += indented + trimmed;
    }
    size_t regionStart = lineStartForLine(firstLine), regionEnd = lineEnd(lineStartForLine(std::min(lastLine, lines.size() - 1)));
    textBuffer.replace(regionStart, regionEnd - regionStart, result);
    selections_.clear(); selections_.emplace_back(regionStart);
    invalidateLineIndex(); dirty_ = true;
}

void Application::shuffleLines() {
    if (largeFileGuarded_ || binaryView_ || selections_.empty()) return;
    size_t a = 0, b = textBuffer.size();
    if (selections_[0].hasSelection()) { a = lineStart(selections_[0].min()); b = lineEnd(selections_[0].max()); }
    std::vector<std::string> lines;
    size_t start = 0;
    std::string block = textBuffer.substr(a, b - a);
    while (true) {
        size_t nl = block.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(block.substr(start)); break; }
        lines.push_back(block.substr(start, nl - start));
        start = nl + 1;
    }
    bool trailing = lines.size() > 1 && lines.back().empty();
    if (trailing) lines.pop_back();
    std::mt19937 rng{std::random_device{}()};
    std::shuffle(lines.begin(), lines.end(), rng);
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) { if (i) joined += '\n'; joined += lines[i]; }
    if (trailing) joined += '\n';
    pushUndo();
    textBuffer.replace(a, b - a, joined);
    selections_.clear(); selections_.emplace_back(a, a + joined.size());
    invalidateLineIndex(); dirty_ = true;
}

// mode: 0 sort, 1 sort (case sensitive), 2 reverse, 3 unique, 4 shuffle
void Application::permuteSelections(int mode) {
    if (largeFileGuarded_ || binaryView_) return;
    std::vector<SelRange> ranges;
    for (auto& s : selections_) if (s.hasSelection()) ranges.push_back(s);
    if (ranges.size() < 2) return;
    std::sort(ranges.begin(), ranges.end(), [](const SelRange& x, const SelRange& y) { return x.min() < y.min(); });
    std::vector<std::string> texts;
    for (auto& r : ranges) texts.push_back(textBuffer.substr(r.min(), r.max() - r.min()));
    if (mode == 0) std::stable_sort(texts.begin(), texts.end(), [](const std::string& x, const std::string& y) { return lowered(x) < lowered(y); });
    else if (mode == 1) std::stable_sort(texts.begin(), texts.end());
    else if (mode == 2) std::reverse(texts.begin(), texts.end());
    else if (mode == 4) { std::mt19937 rng{std::random_device{}()}; std::shuffle(texts.begin(), texts.end(), rng); }
    else if (mode == 3) {
        std::vector<std::string> kept;
        for (auto& t : texts) if (std::find(kept.begin(), kept.end(), t) == kept.end()) kept.push_back(t);
        while (kept.size() < texts.size()) kept.push_back(std::string());
        texts = kept;
    }
    pushUndo();
    std::vector<SelRange> updated;
    long shift = 0;
    for (size_t i = 0; i < ranges.size(); ++i) {
        size_t at = static_cast<size_t>(static_cast<long>(ranges[i].min()) + shift);
        size_t oldLen = ranges[i].max() - ranges[i].min();
        textBuffer.replace(at, oldLen, texts[i]);
        updated.emplace_back(at, at + texts[i].size());
        shift += static_cast<long>(texts[i].size()) - static_cast<long>(oldLen);
    }
    selections_ = updated;
    invalidateLineIndex(); dirty_ = true;
}
