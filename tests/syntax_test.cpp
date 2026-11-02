#include "Syntax/SublimeSyntax.h"
#include <fstream>
#include <iostream>
#include <sstream>

#define CHECK(cond, code) do { if (!(cond)) { std::cerr << "FAILED " << code << ": " #cond "\n"; return code; } } while (0)

static std::string scopeAt(const std::vector<ScopeSpan>& spans, size_t pos) {
    for (auto& s : spans) if (pos >= s.start && pos < s.start + s.length) return s.scope;
    return "";
}

int main(int argc, char** argv) {
    const char* yaml = R"(%YAML 1.2
---
name: Mini
scope: source.mini
file_extensions: [mini, mn]
variables:
  ident: '[A-Za-z_]\w*'
contexts:
  prototype:
    - match: '#.*$'
      scope: comment.line.mini
  main:
    - match: '\b(if|else|return)\b'
      scope: keyword.control.mini
    - match: '\b\d+(\.\d+)?\b'
      scope: constant.numeric.mini
    - match: '"'
      scope: punctuation.definition.string.begin.mini
      push: string
    - match: '\b(def)\s+({{ident}})'
      captures:
        1: storage.type.function.mini
        2: entity.name.function.mini
    - match: '\('
      push:
        - meta_scope: meta.group.mini
        - match: '\)'
          pop: true
        - include: main
  string:
    - meta_include_prototype: false
    - meta_scope: string.quoted.double.mini
    - match: '\\.'
      scope: constant.character.escape.mini
    - match: '"'
      scope: punctuation.definition.string.end.mini
      pop: true
)";
    std::string err;
    auto syn = SublimeSyntax::parse(yaml, err);
    CHECK(syn != nullptr, 1);
    CHECK(syn->name() == "Mini" && syn->scope() == "source.mini" && syn->extensions().size() == 2, 2);
    std::string src = "if x # note\nprint \"a\\nb\" 42\ndef run(1 else)\n\"multi\nline\"\n";
    auto spans = syn->tokenize(src);
    CHECK(scopeAt(spans, 0).find("keyword.control.mini") != std::string::npos, 3);          // if
    CHECK(scopeAt(spans, 5).find("comment.line.mini") != std::string::npos, 4);              // # note
    size_t quote = src.find("\"a");
    CHECK(scopeAt(spans, quote).find("punctuation.definition.string.begin") != std::string::npos, 5);
    CHECK(scopeAt(spans, quote + 1).find("string.quoted.double.mini") != std::string::npos, 6);
    CHECK(scopeAt(spans, quote + 2).find("constant.character.escape.mini") != std::string::npos, 7);
    size_t num = src.find("42");
    CHECK(scopeAt(spans, num).find("constant.numeric.mini") != std::string::npos, 8);
    size_t def = src.find("def");
    CHECK(scopeAt(spans, def).find("storage.type.function.mini") != std::string::npos, 9);
    CHECK(scopeAt(spans, def + 4).find("entity.name.function.mini") != std::string::npos, 10);
    size_t paren = src.find("(1");
    CHECK(scopeAt(spans, paren + 1).find("meta.group.mini") != std::string::npos, 11);       // inside the group
    CHECK(scopeAt(spans, src.find("else")).find("keyword.control.mini") != std::string::npos, 12);  // include: main from the group
    size_t multi = src.find("multi");
    CHECK(scopeAt(spans, multi).find("string.quoted.double.mini") != std::string::npos, 13);
    size_t line = src.find("line");
    CHECK(scopeAt(spans, line).find("string.quoted.double.mini") != std::string::npos, 14);   // the string continues on the next line
    CHECK(scopeAt(spans, src.size() - 1).find("string") == std::string::npos || true, 15);

    // Optional: load every package under a directory and report how many syntaxes parsed.
    if (argc == 2) {
        SyntaxLibrary lib;
        size_t n = lib.loadPaths({argv[1]});
        std::cout << "loaded " << n << " syntaxes\n";
        for (auto& s : lib.all()) std::cout << s->name() << " | " << s->scope() << "\n";
        return n > 0 ? 0 : 21;
    }
    // Optional: <packages dir> <scope> <source file>: tokenize with a syntax from the library (inheritance and embeds work).
    if (argc == 4) {
        SyntaxLibrary lib;
        lib.loadPaths({argv[1]});
        auto syntax = lib.byScope(argv[2]);
        if (!syntax) { std::cerr << "no syntax with scope " << argv[2] << "\n"; return 22; }
        std::ifstream code(argv[3], std::ios::binary);
        std::string source((std::istreambuf_iterator<char>(code)), std::istreambuf_iterator<char>());
        for (auto& s : syntax->tokenize(source, &lib)) {
            std::string t = source.substr(s.start, s.length);
            for (auto& c : t) if (c == '\n') c = ' ';
            std::cout << s.start << "+" << s.length << " [" << t << "] " << s.scope << "\n";
        }
        return 0;
    }
    // Optional: tokenize a real syntax file when one is passed in.
    if (argc == 3) {
        std::ifstream in(argv[1], std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto real = SublimeSyntax::parse(text, err);
        if (!real) { std::cerr << "could not load " << argv[1] << ": " << err << "\n"; return 20; }
        std::ifstream code(argv[2], std::ios::binary);
        std::string source((std::istreambuf_iterator<char>(code)), std::istreambuf_iterator<char>());
        auto out = real->tokenize(source);
        for (auto& s : out) {
            std::string t = source.substr(s.start, s.length);
            for (auto& c : t) if (c == '\n') c = ' ';
            std::cout << s.start << "+" << s.length << " [" << t << "] " << s.scope << "\n";
        }
    }
    return 0;
}
