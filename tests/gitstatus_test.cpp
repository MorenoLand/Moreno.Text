// Parsing `git status --porcelain=v1 -z` output into per-path marks.
#include "Core/GitStatus.h"
#include <iostream>

static int failures = 0;
static void check(const char* name, bool ok) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; }

int main() {
    GitStatus::Map map;
    std::string out;
    out += std::string(" M src/a.cpp\0", 13);
    out += std::string("?? notes.txt\0", 13);
    out += std::string("A  src/new/b.cpp\0", 17);
    out += std::string(" D gone.txt\0", 12);
    out += std::string("R  moved.txt\0old.txt\0", 21);
    out += std::string("UU clash.txt\0", 13);
    out += std::string("!! build/x.o\0", 13);
    GitStatus::parse("C:/Repo", out, map);
    auto at = [&](const char* p) { auto it = map.find(GitStatus::normalize(p)); return it == map.end() ? '-' : it->second; };
    check("modified", at("C:/Repo/src/a.cpp") == 'M');
    check("untracked", at("C:/Repo/notes.txt") == '?');
    check("added", at("C:/Repo/src/new/b.cpp") == 'A');
    check("deleted", at("C:/Repo/gone.txt") == 'D');
    check("renamed uses the new name", at("C:/Repo/moved.txt") == 'R' && at("C:/Repo/old.txt") == '-');
    check("conflict", at("C:/Repo/clash.txt") == 'U');
    check("ignored files are left out", at("C:/Repo/build/x.o") == '-');
    check("folders holding changes are marked", at("C:/Repo/src") == 'M' && at("C:/Repo/src/new") == 'M');
    check("the repository root is marked too", at("C:/Repo") == 'M');
    check("clean files have no mark", at("C:/Repo/readme.md") == '-');
    check("paths compare with either slash", GitStatus::normalize("C:\\Repo\\src\\") == GitStatus::normalize("c:/repo/src") || GitStatus::normalize("C:\\Repo\\src\\") == "C:/Repo/src");
    return failures ? 1 : 0;
}
