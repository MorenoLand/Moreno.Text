// Editing and cursor movement with several selections at once.
#include "Core/Application.h"
#include <SDL2/SDL.h>
#include <algorithm>

#ifdef None
#undef None
#endif

namespace {

// Sorts selections by position and merges any that overlap or touch while one of them has a range.
std::vector<SelRange> mergedSelections(std::vector<SelRange> sels) {
    std::sort(sels.begin(), sels.end(), [](const SelRange& a, const SelRange& b) { return a.min() < b.min(); });
    std::vector<SelRange> out;
    for (const SelRange& s : sels) {
        if (!out.empty() && s.min() < out.back().max()) {
            size_t lo = std::min(out.back().min(), s.min()), hi = std::max(out.back().max(), s.max());
            out.back() = out.back().anchor <= out.back().cursor ? SelRange(lo, hi) : SelRange(hi, lo);
        } else if (!out.empty() && s.min() == out.back().max() && (s.hasSelection() || out.back().hasSelection())) {
            out.push_back(s);
        } else if (!out.empty() && s.min() == out.back().min() && s.max() == out.back().max()) {
            continue;
        } else {
            out.push_back(s);
        }
    }
    return out;
}

size_t prevUtf8(const std::string& text, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

size_t nextUtf8(const std::string& text, size_t pos) {
    if (pos >= text.size()) return text.size();
    ++pos;
    while (pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

}  // namespace

void Application::insertTextMulti(const std::string& text, bool distributeLines) {
    if (binaryView_ || largeFileGuarded_) return;
    std::vector<SelRange> sels = mergedSelections(selections_);
    std::vector<std::string> pieces(sels.size(), text);
    if (distributeLines && sels.size() > 1) {
        std::vector<std::string> lines;
        size_t start = 0;
        while (true) {
            size_t nl = text.find('\n', start);
            std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        if (lines.size() == sels.size() + 1 && lines.back().empty()) lines.pop_back();
        if (lines.size() == sels.size()) pieces = lines;
    }
    pushUndo();
    std::vector<SelRange> updated;
    long shift = 0;
    for (size_t i = 0; i < sels.size(); ++i) {
        size_t a = static_cast<size_t>(static_cast<long>(sels[i].min()) + shift), b = static_cast<size_t>(static_cast<long>(sels[i].max()) + shift);
        textBuffer.replace(a, b - a, pieces[i]);
        shift += static_cast<long>(pieces[i].size()) - static_cast<long>(b - a);
        updated.emplace_back(a + pieces[i].size());
    }
    selections_ = updated;
    invalidateLineIndex(); dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true; maxLineWidthDirty_ = true;
    desiredCursorX_ = -1.f;
    ensureCursorVisible();
}

void Application::deleteMulti(bool forward) {
    if (binaryView_ || largeFileGuarded_) return;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (const SelRange& s : selections_) {
        if (s.hasSelection()) ranges.emplace_back(s.min(), s.max());
        else if (forward) { size_t e = nextUtf8(textBuffer, s.cursor); if (e > s.cursor) ranges.emplace_back(s.cursor, e); else ranges.emplace_back(s.cursor, s.cursor); }
        else { size_t b = prevUtf8(textBuffer, s.cursor); ranges.emplace_back(b, s.cursor); }
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<size_t, size_t>> merged;
    for (auto& r : ranges) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    pushUndo();
    std::vector<SelRange> updated;
    long shift = 0;
    for (auto& r : merged) {
        size_t a = static_cast<size_t>(static_cast<long>(r.first) + shift), b = static_cast<size_t>(static_cast<long>(r.second) + shift);
        textBuffer.erase(a, b - a);
        shift -= static_cast<long>(b - a);
        updated.emplace_back(a);
    }
    selections_ = updated;
    invalidateLineIndex(); dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true; maxLineWidthDirty_ = true;
    desiredCursorX_ = -1.f;
}

// Moves every cursor; `key` is an SDL keycode (left/right/up/down/home/end).
void Application::moveAllCursors(int key, bool extend) {
    for (SelRange& s : selections_) {
        size_t pos = s.cursor;
        size_t ls = lineStart(pos), le = lineEnd(pos);
        switch (key) {
            case SDLK_LEFT:
                if (!extend && s.hasSelection()) pos = s.min(); else pos = prevUtf8(textBuffer, pos);
                break;
            case SDLK_RIGHT:
                if (!extend && s.hasSelection()) pos = s.max(); else pos = nextUtf8(textBuffer, pos);
                break;
            case SDLK_HOME: pos = ls; break;
            case SDLK_END: pos = le; break;
            case SDLK_UP:
            case SDLK_DOWN: {
                size_t col = pos - ls;
                if (key == SDLK_UP) {
                    if (ls == 0) { pos = 0; break; }
                    size_t prevStart = lineStart(ls - 1);
                    pos = prevStart + std::min(col, (ls - 1) - prevStart);
                } else {
                    if (le >= textBuffer.size()) { pos = textBuffer.size(); break; }
                    size_t nextStart = le + 1, nextEnd = lineEnd(nextStart);
                    pos = nextStart + std::min(col, nextEnd - nextStart);
                }
                break;
            }
            default: break;
        }
        s.cursor = pos;
        if (!extend) s.anchor = pos;
    }
    selections_ = mergedSelections(selections_);
    desiredCursorX_ = -1.f;
    ensureCursorVisible();
}
