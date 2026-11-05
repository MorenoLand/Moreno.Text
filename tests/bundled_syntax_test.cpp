// Loads the syntaxes that ship in Syntaxes/ and checks that every file parses, the common extensions resolve,
// and a sample in each language tokenizes quickly into the scopes you would expect.
#include "Syntax/SublimeSyntax.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

struct Sample { const char* ext; const char* code; const char* expectScope; };

static const Sample samples[] = {
    {"go", "package main\n\nimport \"fmt\"\n\n// say hi\nfunc main() {\n\tfmt.Println(\"hi\", 42)\n}\n", "keyword"},
    {"rs", "fn main() {\n    let x: u32 = 5; // note\n    println!(\"{}\", x);\n}\n", "keyword"},
    {"java", "public class A {\n  /** doc */\n  public static void main(String[] a) { System.out.println(\"x\"); }\n}\n", "storage"},
    {"html", "<!DOCTYPE html>\n<html><head><style>a{color:red}</style><script>var x = 1;</script></head><body class=\"a\">hi</body></html>\n", "entity.name.tag"},
    {"css", "a.b > c { color: #fff; margin: 0 auto; } /* c */\n@media (max-width: 3px) { d { top: 1px } }\n", "support.type.property-name"},
    {"php", "<?php\nfunction f($a) { return $a . \"x\"; }\n", "keyword"},
    {"sql", "SELECT a, b FROM t WHERE c = 'x' -- note\nORDER BY a;\n", "keyword"},
    {"sh", "#!/bin/bash\nfor i in 1 2; do echo \"$i\"; done # c\n", "keyword"},
    {"rb", "class A\n  def b(x)\n    puts \"#{x}\"\n  end\nend\n", "keyword"},
    {"lua", "local function f(a) return a + 1 end -- c\n", "keyword"},
    {"py", "def f(x):\n    return x + 1  # c\n", "keyword"},
    {"js", "const f = (a) => a + 1; // c\n", "keyword"},
    {"ts", "interface A { b: number }\nconst a: A = { b: 1 };\n", "keyword"},
    {"cs", "class A { void B() { var x = \"s\"; } }\n", "storage"},
    {"cpp", "#include <vector>\nint main() { return 0; } // c\n", "keyword"},
    {"md", "# Title\n\nsome *text* and `code`\n\n```py\nx = 1\n```\n", "markup"},
    {"json", "{\"a\": [1, true, null], \"b\": \"c\"}\n", "constant"},
    {"yml", "a: 1\nb:\n  - x\n  - \"y\"\n# c\n", "string"},
    {"toml", "[a]\nb = 1\nc = \"d\" # e\n", "string"},
    {"xml", "<?xml version=\"1.0\"?><a b=\"c\"><d/></a>\n", "entity.name.tag"},
    {"bat", "@echo off\nset A=1\nif %A%==1 echo hi\n", "keyword"},
    {"diff", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n", "markup"},
    {"gs2", "function onCreated() {\n  echo(\"hi\");\n}\n", "keyword"},
};

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    if (argc < 2) { std::cerr << "usage: bundled_syntax_test <Syntaxes dir>\n"; return 2; }
    fs::path dir = argv[1];
    int failures = 0;
    size_t files = 0, parsed = 0;
    std::vector<std::pair<std::string, std::string>> parsedFiles;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".sublime-syntax") continue;
        ++files;
        std::ifstream in(e.path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        if (auto syn = SublimeSyntax::parse(text, err)) { ++parsed; parsedFiles.push_back({e.path().filename().string(), syn->name() + "|" + syn->scope()}); }
        else { std::cout << "FAIL parse " << e.path().filename().string() << ": " << err << "\n"; ++failures; }
    }
    std::cout << "parsed " << parsed << " of " << files << " syntax files\n";
    auto t0 = std::chrono::steady_clock::now();
    SyntaxLibrary lib;
    lib.loadPaths({dir.string()});
    double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (argc > 2 && std::string(argv[2]) == "--regexes") {   // dev aid: every pattern that fails to compile
        std::set<std::string> seen;
        for (auto& line : lib.regexProblems()) if (seen.insert(line).second) std::cout << line << std::endl;
        std::cout << seen.size() << " distinct problems" << std::endl;
        return 0;
    }
    if (argc > 4 && std::string(argv[2]) == "--tokenize") {   // dev aid: print the scopes of a file, one span per line
        auto syn = lib.byExtension(argv[3]);
        std::ifstream in(argv[4], std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!syn) { std::cout << "no syntax for ." << argv[3] << '\n'; return 1; }
        for (auto& sp : syn->tokenize(text, &lib)) std::cout << sp.start << "+" << sp.length << " [" << text.substr(sp.start, std::min<size_t>(sp.length, 24)) << "] " << sp.scope << '\n';
        return 0;
    }
    std::cout << "library holds " << lib.size() << " syntaxes, loaded in " << loadMs << " ms\n";
    {
        std::set<std::string> have;
        for (auto& syn : lib.all()) have.insert(syn->name() + "|" + syn->scope());
        for (auto& pf : parsedFiles) if (!have.count(pf.second)) std::cout << "dropped: " << pf.first << "\n";
    }
    if (lib.size() < files) { std::cout << "FAIL some syntaxes were dropped while loading\n"; ++failures; }

    const size_t bigSize = argc > 3 ? static_cast<size_t>(std::atoi(argv[3])) * 1000 : 200000;
    for (const Sample& s : samples) {
        if (argc > 2 && std::string(s.ext) != argv[2]) continue;
        auto syn = lib.byExtension(s.ext);
        if (!syn) { std::cout << "FAIL no syntax for ." << s.ext << "\n"; ++failures; continue; }
        std::string text = s.code;
        auto start = std::chrono::steady_clock::now();
        auto spans = syn->tokenize(text, &lib);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        bool hit = false;
        for (auto& sp : spans) if (sp.scope.find(s.expectScope) != std::string::npos) { hit = true; break; }
        std::cout << (hit ? "PASS " : "FAIL ") << "." << s.ext << " -> " << syn->name() << " (" << spans.size() << " spans, " << ms << " ms)\n";
        if (!hit) ++failures;
        // a larger file must stay fast: repeat the sample until it is about 200 KB
        std::string big;
        while (big.size() < bigSize) big += text;
        start = std::chrono::steady_clock::now();
        auto bigSpans = syn->tokenize(big, &lib);
        double bigMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "     " << big.size() / 1000 << " KB in " << bigMs << " ms\n";
        if (bigMs > 8000.0) { std::cout << "FAIL ." << s.ext << " took " << bigMs << " ms for 200 KB\n"; ++failures; }
        (void)bigSpans;
    }
    return failures ? 1 : 0;
}
