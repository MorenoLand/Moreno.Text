#include "Core/SpellChecker.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond, code) do { if (!(cond)) { std::cerr << "FAILED: " #cond "\n"; return code; } } while (0)

int main() {
    fs::path dir = fs::temp_directory_path() / "moreno_spell_test";
    fs::create_directories(dir);
    {
        std::ofstream(dir / "t.aff") << "SET UTF-8\nPFX U Y 1\nPFX U 0 un .\nSFX S Y 2\nSFX S y ies [^aeiou]y\nSFX S 0 s [^y]\nSFX G Y 1\nSFX G e ing e\n";
        std::ofstream(dir / "t.dic") << "4\ncat/S\ncity/S\nmake/GU\nhello\n";
    }
    SpellChecker sc;
    CHECK(sc.load((dir / "t.dic").string()), 1);
    CHECK(sc.isCorrect("cat"), 2);
    CHECK(sc.isCorrect("cats"), 3);
    CHECK(sc.isCorrect("cities"), 4);
    CHECK(sc.isCorrect("making"), 5);
    CHECK(sc.isCorrect("unmake"), 6);
    CHECK(sc.isCorrect("unmaking"), 7);
    CHECK(sc.isCorrect("Hello"), 8);
    CHECK(sc.isCorrect("HELLO"), 9);
    CHECK(!sc.isCorrect("catz"), 10);
    CHECK(!sc.isCorrect("citys"), 11);
    CHECK(sc.isCorrect("abc123"), 12);
    auto suggestions = sc.suggest("catz");
    CHECK(std::find(suggestions.begin(), suggestions.end(), "cat") != suggestions.end(), 13);
    sc.addWord("zyzzyva");
    CHECK(sc.isCorrect("Zyzzyva"), 14);
    fs::remove_all(dir);
    return 0;
}
