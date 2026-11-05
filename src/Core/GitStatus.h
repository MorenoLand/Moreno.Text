// What `git status` says about the files under a folder, for the sidebar. Runs git itself, so it needs git on the PATH.
#pragma once
#include <string>
#include <unordered_map>

namespace GitStatus {
    // One letter per path: M modified, A added, D deleted, R renamed, ? untracked, U conflict.
    // Folders that contain changes are marked M. Keys are normalize()d absolute paths.
    using Map = std::unordered_map<std::string, char>;

    // Forward slashes, no trailing slash, lower case on Windows: the key every lookup uses.
    std::string normalize(const std::string& path);
    // Reads `git status --porcelain=v1 -z` output for the repository at `root` into `out`.
    void parse(const std::string& root, const std::string& porcelain, Map& out);
    // Runs git for the repository around `dir` (nothing is added when there is none).
    void collect(const std::string& dir, Map& out);
}
