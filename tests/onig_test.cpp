#include "Syntax/OnigRegex.h"
#include <iostream>

#define CHECK(cond, code) do { if (!(cond)) { std::cerr << "FAILED " << code << ": " #cond "\n"; return code; } } while (0)

static bool rfind_(const char* pattern, const std::string& text, std::string& whole, size_t from = 0, size_t anchor = 0) {
    std::string err;
    auto re = OnigRegex::compile(pattern, err);
    if (!re) { std::cerr << "compile error for " << pattern << ": " << err << "\n"; return false; }
    OnigRegex::Match m;
    if (!re->search(text, from, anchor, m)) return false;
    whole = text.substr(static_cast<size_t>(m.groups[0].first), static_cast<size_t>(m.groups[0].second - m.groups[0].first));
    return true;
}

int main(int argc, char** argv) {
    std::string w;
    if (argc == 3) { bool ok = rfind_(argv[1], argv[2], w); std::cout << (ok ? "match [" + w + "]" : std::string("no match")) << '\n'; return 0; }
    CHECK(rfind_("fo+", "xfoooy", w) && w == "fooo", 1);
    CHECK(rfind_("\\b(?:if|else)\\b", "elsewhere if x", w) && w == "if", 2);
    CHECK(rfind_("[a-z_]\\w*", "123 abc_9 x", w) && w == "abc_9", 3);
    CHECK(rfind_("(?<=\\$)\\w+", "cost $price now", w) && w == "price", 4);
    CHECK(rfind_("\\w+(?=\\()", "x = call(1)", w) && w == "call", 5);
    CHECK(!rfind_("\\w+(?!\\w)(?=\\()", "x = y", w), 6);
    CHECK(rfind_("(['\"]).*?\\1", "say \"hi\" and 'no'", w) && w == "\"hi\"", 7);
    CHECK(rfind_("(?i)hello", "Say HELLO", w) && w == "HELLO", 8);
    CHECK(rfind_("^\\s*#\\s*include", "  # include <x>", w), 9);
    CHECK(rfind_("[[:digit:]]+\\.[[:digit:]]*", "pi 3.14!", w) && w == "3.14", 10);
    CHECK(rfind_("(?x) \\d+ # digits\n \\s* px", "10 px", w) && w == "10 px", 11);
    CHECK(rfind_("a++b", "aaab", w) && w == "aaab", 12);
    CHECK(!rfind_("a++ab", "aaab", w), 13);
    CHECK(rfind_("(?>a+)b", "aab", w) && w == "aab", 14);
    CHECK(rfind_("\\h+", "xyz 1f2e", w) && w == "1f2e", 15);
    CHECK(rfind_("//.*$", "x // note\ny", w) && w == "// note", 16);
    CHECK(rfind_("\\Gx", "xx", w, 0, 0) && w == "x", 17);
    CHECK(!rfind_("\\Gx", "ax", w, 0, 0), 18);
    CHECK(rfind_("(?<name>\\w+)=\\k<name>", "ab=ab", w) && w == "ab=ab", 19);
    CHECK(rfind_("[^\\s]+", "  word  ", w) && w == "word", 20);
    CHECK(rfind_("\\p{Alpha}+", "12 abc", w) && w == "abc", 21);
    CHECK(rfind_("colou?r", "my color", w) && w == "color", 22);
    CHECK(rfind_("a{2,3}", "aaaa", w) && w == "aaa", 23);
    std::string err;
    CHECK(!OnigRegex::compile("(unclosed", err) && !err.empty(), 24);
    return 0;
}
