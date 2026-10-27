#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Reads Hunspell dictionaries (.dic with a sibling .aff): word lists with affix flags and
// prefix/suffix rules. Compound rules and morphological fields are ignored.
class SpellChecker {
public:
    bool load(const std::string& dicPath);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }
    bool isCorrect(const std::string& word) const;
    void addWord(const std::string& word);
    std::vector<std::string> suggest(const std::string& word, size_t limit = 8) const;

private:
    struct Condition {
        std::vector<std::string> sets;  // each entry is one position: "x", or "[abc]" / "[^abc]" / "."
    };
    struct Rule {
        bool prefix = false;
        bool cross = false;
        std::string strip, add;
        Condition cond;
        char flag = 0;
    };
    bool matchesCondition(const Rule& rule, const std::string& stem) const;
    const std::string* lookup(const std::string& word) const;
    bool hasBase(const std::string& word, char requiredFlag, char secondFlag) const;
    bool checkVariants(const std::string& word) const;
    void loadAffixes(const std::string& affPath);

    bool loaded_ = false;
    std::string path_;
    std::unordered_map<std::string, std::string> words_;  // word -> flags
    std::vector<Rule> rules_;
    std::unordered_set<std::string> added_;
};
