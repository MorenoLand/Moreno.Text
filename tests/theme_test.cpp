#include "Theme/ThemeEngine.h"
#include <cmath>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

static bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

int main() {
    fs::path dir = fs::temp_directory_path() / "moreno_theme_test";
    fs::create_directories(dir);
    {
        std::ofstream(dir / "a.sublime-color-scheme") << R"JSON({
            // comment
            "name": "Test",
            "variables": { "red": "#ff0000", "bg": "#101010" },
            "globals": { "background": "var(bg)", "foreground": "#ffffff", "selection": "color(var(red) alpha(0.5))", },
            "rules": [
                { "scope": "keyword.control, storage", "foreground": "var(red)" },
                { "scope": "string", "foreground": "#00ff00" },
            ]
        })JSON";
        auto& t = ThemeEngine::instance();
        t.load((dir / "a.sublime-color-scheme").string());
        if (t.name() != "Test") { std::cerr << "name\n"; return 1; }
        if (!near(t.bgColor().r, 16 / 255.f)) { std::cerr << "variable background\n"; return 2; }
        if (!near(t.selectionColor().a, 0.5f) || !near(t.selectionColor().r, 1.f)) { std::cerr << "color() alpha\n"; return 3; }
        if (!near(t.colorForScope("keyword").r, 1.f)) { std::cerr << "coarse keyword borrowed from keyword.control\n"; return 4; }
        if (!near(t.colorForScope("string").g, 1.f)) { std::cerr << "string rule\n"; return 5; }
    }
    {
        std::ofstream(dir / "b.tmTheme") << R"XML(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>name</key><string>Legacy</string>
  <key>settings</key>
  <array>
    <dict><key>settings</key><dict>
      <key>background</key><string>#272822</string><key>foreground</key><string>#F8F8F2</string><key>caret</key><string>#F8F8F0</string>
    </dict></dict>
    <dict><key>name</key><string>Comment</string><key>scope</key><string>comment</string>
      <key>settings</key><dict><key>foreground</key><string>#75715E</string></dict></dict>
  </array>
</dict></plist>)XML";
        auto& t = ThemeEngine::instance();
        t.load((dir / "b.tmTheme").string());
        if (t.name() != "Legacy") { std::cerr << "tm name\n"; return 6; }
        if (!near(t.bgColor().r, 0x27 / 255.f)) { std::cerr << "tm background\n"; return 7; }
        if (!near(t.colorForScope("comment").r, 0x75 / 255.f)) { std::cerr << "tm comment\n"; return 8; }
    }
    {   // full scope stacks, picked the way Sublime picks: deepest and most specific rule wins
        auto& t = ThemeEngine::instance();
        t.writeDefault((dir / "Default.moreno-color-scheme").string());
        t.load((dir / "Default.moreno-color-scheme").string());
        auto stack = [&](const char* s) { return t.styleForStack(s).color; };
        auto same = [&](ThemeColor a, ThemeColor b) { return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b); };
        if (same(stack("source.go keyword.declaration.go"), t.fgColor())) { std::cerr << "keyword.declaration should be coloured\n"; return 9; }
        if (!same(stack("source.go keyword.declaration.go"), t.colorForScope("keyword"))) { std::cerr << "keyword.declaration uses the keyword colour\n"; return 10; }
        if (!same(stack("text.html.basic meta.tag.block.any.html entity.name.tag.block.any.html"), t.colorForScope("tag"))) { std::cerr << "html tag name uses the tag colour\n"; return 11; }
        if (!same(stack("source.go meta.function.go entity.name.function.go"), t.colorForScope("function"))) { std::cerr << "function name uses the function colour\n"; return 12; }
        if (!same(stack("source.go comment.line.double-slash.go punctuation.definition.comment.go"), t.colorForScope("comment"))) { std::cerr << "comment punctuation stays comment coloured\n"; return 13; }
        if (!same(stack("source.go meta.block.go"), t.fgColor())) { std::cerr << "plain code keeps the foreground\n"; return 14; }
        if (!same(stack("source.java meta.class.java storage.modifier.java"), t.colorForScope("keyword"))) { std::cerr << "storage.modifier uses the keyword colour\n"; return 15; }
        if (!same(stack("source.java meta.class.java meta.block.java meta.function.java storage.type.primitive.java"), t.colorForScope("type"))) { std::cerr << "storage.type.primitive uses the type colour\n"; return 16; }
    }
    fs::remove_all(dir);
    return 0;
}
