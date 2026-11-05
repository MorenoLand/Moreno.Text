// file_exclude_patterns, folder_exclude_patterns and binary_file_patterns.
#include "Core/ExcludeRules.h"
#include "Settings/SettingsManager.h"
#include <filesystem>
#include <fstream>
#include <iostream>

static int failures = 0;
static void check(const char* name, bool ok) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; }

int main() {
    check("star matches a suffix", ExcludeRules::glob("*.pyc", "main.pyc"));
    check("star does not match another suffix", !ExcludeRules::glob("*.pyc", "main.py"));
    check("question mark matches one character", ExcludeRules::glob("a?c", "abc") && !ExcludeRules::glob("a?c", "ac"));
    check("character set", ExcludeRules::glob("file[0-9].txt", "file7.txt") && !ExcludeRules::glob("file[0-9].txt", "fileX.txt"));
    check("star in the middle", ExcludeRules::glob(".Trash-*", ".Trash-1000") && ExcludeRules::glob("a*z", "abcz") && !ExcludeRules::glob("a*z", "abcy"));
    check("matching ignores case on Windows paths", ExcludeRules::glob("*.EXE", "setup.exe") == (
#ifdef _WIN32
        true
#else
        false
#endif
    ));
    check("a pattern with a slash matches the whole path", ExcludeRules::matches("*/node_modules/*", "x.js", "C:/p/node_modules/x.js") && !ExcludeRules::matches("*/node_modules/*", "x.js", "C:/p/src/x.js"));
    check("a pattern without a slash matches the name", ExcludeRules::matches("*.log", "a.log", "C:/deep/dir/a.log"));

    ExcludeRules d = ExcludeRules::defaults();
    check("defaults hide .git", d.excludesFolder(".git", "C:/p/.git"));
    check("defaults keep src", !d.excludesFolder("src", "C:/p/src"));
    check("defaults hide .pyc and .exe", d.excludesFile("a.pyc", "C:/p/a.pyc") && d.excludesFile("tool.exe", "C:/p/tool.exe"));
    check("defaults keep .png in the sidebar but mark it binary", !d.excludesFile("a.png", "C:/p/a.png") && d.isBinary("a.png", "C:/p/a.png"));

    ExcludeRules extra;
    extra.merge(nlohmann::json::parse(R"({"folder_exclude_patterns": ["vendor"], "file_exclude_patterns": ["*.tmp"]})"));
    d.add(extra);
    check("a project adds patterns on top", d.excludesFolder("vendor", "C:/p/vendor") && d.excludesFile("x.tmp", "C:/p/x.tmp") && d.excludesFolder(".git", "C:/p/.git"));

    auto dir = std::filesystem::temp_directory_path() / "moreno_exclude_test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "Preferences.sublime-settings") << R"({"file_exclude_patterns": ["*.bak"], "folder_exclude_patterns": []})"; }
    SettingsManager::instance().load((dir / "Preferences.sublime-settings").string());
    ExcludeRules fromSettings = ExcludeRules::fromSettings();
    check("settings replace the file list", fromSettings.excludesFile("a.bak", "C:/p/a.bak") && !fromSettings.excludesFile("a.pyc", "C:/p/a.pyc"));
    check("an empty list in settings turns folder excludes off", !fromSettings.excludesFolder(".git", "C:/p/.git"));
    check("a list the settings do not mention keeps its default", fromSettings.isBinary("a.png", "C:/p/a.png"));
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
