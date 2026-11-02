#pragma once
// A small backtracking regular expression engine covering the Oniguruma features that .sublime-syntax files use:
// lookahead and lookbehind, atomic groups, possessive and lazy quantifiers, named groups, backreferences, POSIX
// classes, \h, \G and the (?i) (?x) inline flags. Text is matched as UTF-8 bytes.
#include <memory>
#include <string>
#include <vector>

class OnigRegex {
public:
    struct Match {
        std::vector<std::pair<long, long>> groups;  // [start, end) per group, (-1, -1) when unset; group 0 is the whole match
    };

    // Returns nullptr and fills error when the pattern is not valid.
    static std::shared_ptr<OnigRegex> compile(const std::string& pattern, std::string& error);
    // Finds the first match at or after `from`. `anchor` is the position \G matches at.
    bool search(const std::string& text, size_t from, size_t anchor, Match& out) const;
    int groupCount() const { return groupCount_; }

    struct Node;
    ~OnigRegex();

private:
    OnigRegex() = default;
    friend struct Compiler;
    std::vector<std::unique_ptr<Node>> nodes_;
    int root_ = -1;
    int groupCount_ = 0;
};
