// Editing rules shared by the single-line input boxes (find, replace, goto, command palette, plugin panels).
// The text lives in a std::string owned by the panel; the caret and selection anchor are byte offsets beside it.
#pragma once
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace LineEdit {

inline size_t prevChar(const std::string& s, size_t p) {
    if (!p) return 0;
    --p;
    while (p && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return p;
}

inline size_t nextChar(const std::string& s, size_t p) {
    if (p >= s.size()) return s.size();
    ++p;
    while (p < s.size() && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) ++p;
    return p;
}

inline bool isWord(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || static_cast<unsigned char>(c) >= 0x80; }

inline size_t wordLeft(const std::string& s, size_t p) {
    while (p && !isWord(s[prevChar(s, p)])) p = prevChar(s, p);
    while (p && isWord(s[prevChar(s, p)])) p = prevChar(s, p);
    return p;
}

inline size_t wordRight(const std::string& s, size_t p) {
    while (p < s.size() && !isWord(s[p])) p = nextChar(s, p);
    while (p < s.size() && isWord(s[p])) p = nextChar(s, p);
    return p;
}

inline void clamp(const std::string& s, size_t& cur, size_t& anc) { cur = std::min(cur, s.size()); anc = std::min(anc, s.size()); }
inline size_t selStart(size_t cur, size_t anc) { return std::min(cur, anc); }
inline size_t selEnd(size_t cur, size_t anc) { return std::max(cur, anc); }

inline void eraseSelection(std::string& s, size_t& cur, size_t& anc) {
    size_t first = selStart(cur, anc), last = selEnd(cur, anc);
    s.erase(first, last - first);
    cur = anc = first;
}

inline void insert(std::string& s, size_t& cur, size_t& anc, const std::string& text) {
    clamp(s, cur, anc);
    if (cur != anc) eraseSelection(s, cur, anc);
    s.insert(cur, text);
    cur = anc = cur + text.size();
}

inline void setText(std::string& s, size_t& cur, size_t& anc, const std::string& text) { s = text; cur = anc = s.size(); }

inline void selectWordAt(const std::string& s, size_t pos, size_t& cur, size_t& anc) {
    pos = std::min(pos, s.size());
    size_t a = pos, b = pos;
    while (a && isWord(s[prevChar(s, a)])) a = prevChar(s, a);
    while (b < s.size() && isWord(s[b])) b = nextChar(s, b);
    if (a == b) { a = prevChar(s, pos); b = nextChar(s, pos); }
    anc = a; cur = b;
}

// The caret offset nearest to a pixel x measured from the start of the text; measure(prefix) gives a prefix width.
template <typename Measure>
size_t offsetAtX(const std::string& s, float x, Measure measure) {
    if (x <= 0.f) return 0;
    size_t prev = 0;
    float prevW = 0.f;
    for (size_t p = nextChar(s, 0);; p = nextChar(s, p)) {
        float w = measure(s.substr(0, p));
        if (w >= x) return (x - prevW < w - x) ? prev : p;
        if (p >= s.size()) return s.size();
        prev = p; prevW = w;
    }
}

// 0 = not handled, 1 = handled (caret or selection only), 2 = handled and the text changed.
inline int key(std::string& s, size_t& cur, size_t& anc, const SDL_KeyboardEvent& k) {
    clamp(s, cur, anc);
    const bool ctrl = (k.keysym.mod & KMOD_CTRL) != 0, shift = (k.keysym.mod & KMOD_SHIFT) != 0, alt = (k.keysym.mod & KMOD_ALT) != 0;
    auto move = [&](size_t p) { cur = p; if (!shift) anc = p; return 1; };
    switch (k.keysym.sym) {
    case SDLK_LEFT:
        if (cur != anc && !shift && !ctrl) return move(selStart(cur, anc));
        return move(ctrl ? wordLeft(s, cur) : prevChar(s, cur));
    case SDLK_RIGHT:
        if (cur != anc && !shift && !ctrl) return move(selEnd(cur, anc));
        return move(ctrl ? wordRight(s, cur) : nextChar(s, cur));
    case SDLK_HOME: return move(0);
    case SDLK_END: return move(s.size());
    case SDLK_BACKSPACE:
        if (cur != anc) { eraseSelection(s, cur, anc); return 2; }
        if (!cur) return 1;
        { size_t start = ctrl ? wordLeft(s, cur) : prevChar(s, cur); s.erase(start, cur - start); cur = anc = start; }
        return 2;
    case SDLK_DELETE:
        if (cur != anc) { eraseSelection(s, cur, anc); return 2; }
        if (cur >= s.size()) return 1;
        { size_t end = ctrl ? wordRight(s, cur) : nextChar(s, cur); s.erase(cur, end - cur); anc = cur; }
        return 2;
    default: break;
    }
    if (!ctrl || alt) return 0;
    switch (k.keysym.sym) {
    case SDLK_a: anc = 0; cur = s.size(); return 1;
    case SDLK_c:
        if (cur != anc) SDL_SetClipboardText(s.substr(selStart(cur, anc), selEnd(cur, anc) - selStart(cur, anc)).c_str());
        return 1;
    case SDLK_x:
        if (cur == anc) return 1;
        SDL_SetClipboardText(s.substr(selStart(cur, anc), selEnd(cur, anc) - selStart(cur, anc)).c_str());
        eraseSelection(s, cur, anc);
        return 2;
    case SDLK_v: {
        char* clip = SDL_GetClipboardText();
        std::string text = clip ? clip : "";
        if (clip) SDL_free(clip);
        for (char& c : text) if (c == '\r' || c == '\n') c = ' ';
        if (text.empty()) return 1;
        insert(s, cur, anc, text);
        return 2;
    }
    default: return 0;
    }
}

}  // namespace LineEdit
