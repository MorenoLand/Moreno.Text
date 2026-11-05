// Keyboard and caret rules shared by the single-line input boxes.
#include "UI/LineEdit.h"
#include <cstdio>

static int failures = 0;
static void check(const char* name, bool ok) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }

static SDL_KeyboardEvent press(SDL_Keycode sym, Uint16 mod = 0) {
    SDL_KeyboardEvent k{};
    k.type = SDL_KEYDOWN; k.keysym.sym = sym; k.keysym.mod = mod;
    return k;
}

int main() {
    std::string s = "hello world foo";
    size_t cur = s.size(), anc = s.size();
    check("ctrl+a selects everything", LineEdit::key(s, cur, anc, press(SDLK_a, KMOD_CTRL)) == 1 && anc == 0 && cur == s.size());
    check("backspace removes the selection", LineEdit::key(s, cur, anc, press(SDLK_BACKSPACE)) == 2 && s.empty() && cur == 0);

    s = "hello world foo"; cur = anc = s.size();
    LineEdit::key(s, cur, anc, press(SDLK_LEFT, KMOD_CTRL));
    check("ctrl+left jumps a word", cur == 12 && anc == 12);
    LineEdit::key(s, cur, anc, press(SDLK_LEFT, KMOD_CTRL | KMOD_SHIFT));
    check("ctrl+shift+left extends the selection", cur == 6 && anc == 12);
    LineEdit::key(s, cur, anc, press(SDLK_DELETE));
    check("delete removes the selected word", s == "hello foo" && cur == 6);
    LineEdit::key(s, cur, anc, press(SDLK_BACKSPACE, KMOD_CTRL));
    check("ctrl+backspace removes the word before the caret", s == "foo" && cur == 0);
    LineEdit::key(s, cur, anc, press(SDLK_END, KMOD_SHIFT));
    check("shift+end selects to the end", cur == 3 && anc == 0);
    LineEdit::key(s, cur, anc, press(SDLK_LEFT));
    check("left collapses a selection to its start", cur == 0 && anc == 0);

    s = "caf\xc3\xa9!"; cur = anc = s.size();
    LineEdit::key(s, cur, anc, press(SDLK_LEFT));
    LineEdit::key(s, cur, anc, press(SDLK_BACKSPACE));
    check("backspace removes a whole UTF-8 character", s == "caf!");

    s = "abc"; cur = 1; anc = 3;
    LineEdit::insert(s, cur, anc, "XY");
    check("typing replaces the selection", s == "aXY" && cur == 3 && anc == 3);

    s = "one two"; cur = anc = 0;
    LineEdit::selectWordAt(s, 5, cur, anc);
    check("double-click selects the word under the pointer", anc == 4 && cur == 7);

    auto measure = [](const std::string& t) { return 10.f * (float)t.size(); };
    check("a click lands on the nearest caret position", LineEdit::offsetAtX("abcd", 24.f, measure) == 2 && LineEdit::offsetAtX("abcd", 26.f, measure) == 3 && LineEdit::offsetAtX("abcd", 500.f, measure) == 4 && LineEdit::offsetAtX("abcd", -4.f, measure) == 0);
    return failures ? 1 : 0;
}
