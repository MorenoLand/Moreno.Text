#include "Core/SpellChecker.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool isCapitalized(const std::string& w) {
    return !w.empty() && std::isupper(static_cast<unsigned char>(w[0])) && std::all_of(w.begin() + 1, w.end(), [](unsigned char c) { return !std::isupper(c); });
}

bool isAllUpper(const std::string& w) {
    bool any = false;
    for (unsigned char c : w) { if (std::isupper(c)) any = true; else if (std::islower(c)) return false; }
    return any;
}

}  // namespace

void SpellChecker::loadAffixes(const std::string& affPath) {
    std::ifstream in(affPath, std::ios::binary);
    if (!in) return;
    std::string line;
    if (in.peek() == 0xEF) { char bom[3]; in.read(bom, 3); }
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kind, flag, a, b, c;
        ls >> kind;
        if (kind != "SFX" && kind != "PFX") continue;
        ls >> flag >> a >> b;
        if (flag.empty()) continue;
        if (a == "Y" || a == "N") {
            if (b.empty() || std::isdigit(static_cast<unsigned char>(b[0]))) continue;  // header: SFX A Y 3
        }
        // rule: SFX flag strip add [condition]
        Rule rule;
        rule.prefix = kind == "PFX";
        rule.flag = flag[0];
        rule.strip = a == "0" ? std::string() : a;
        size_t slash = b.find('/');
        std::string add = slash == std::string::npos ? b : b.substr(0, slash);
        rule.add = add == "0" ? std::string() : add;
        ls >> c;
        std::string cond = c.empty() ? "." : c;
        for (size_t i = 0; i < cond.size();) {
            if (cond[i] == '[') {
                size_t close = cond.find(']', i);
                if (close == std::string::npos) close = cond.size() - 1;
                rule.cond.sets.push_back(cond.substr(i, close - i + 1));
                i = close + 1;
            } else {
                rule.cond.sets.push_back(std::string(1, cond[i]));
                ++i;
            }
        }
        rules_.push_back(std::move(rule));
    }
    // Cross-product flag comes from the header lines.
    std::ifstream again(affPath, std::ios::binary);
    std::unordered_map<std::string, bool> cross;
    while (std::getline(again, line)) {
        std::istringstream ls(line);
        std::string kind, flag, yn, count;
        ls >> kind >> flag >> yn >> count;
        if ((kind == "SFX" || kind == "PFX") && (yn == "Y" || yn == "N") && !count.empty() && std::isdigit(static_cast<unsigned char>(count[0])))
            cross[kind + flag] = yn == "Y";
    }
    for (auto& r : rules_) {
        auto it = cross.find(std::string(r.prefix ? "PFX" : "SFX") + std::string(1, r.flag));
        r.cross = it != cross.end() && it->second;
    }
}

bool SpellChecker::load(const std::string& dicPath) {
    std::ifstream in(dicPath, std::ios::binary);
    if (!in) return false;
    words_.clear(); rules_.clear();
    std::string aff = dicPath;
    size_t dot = aff.rfind('.');
    if (dot != std::string::npos) aff = aff.substr(0, dot) + ".aff";
    loadAffixes(aff);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            std::string t = trimmed(line);
            if (!t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
            if (t.size() >= 3 && (unsigned char)t[0] == 0xEF) line = line.substr(3);
        }
        size_t cut = line.find_first_of(" \t\r");
        std::string entry = cut == std::string::npos ? line : line.substr(0, cut);
        if (entry.empty()) continue;
        size_t slash = entry.find('/');
        std::string word = slash == std::string::npos ? entry : entry.substr(0, slash);
        std::string flags = slash == std::string::npos ? std::string() : entry.substr(slash + 1);
        auto& slot = words_[word];
        slot += flags;
    }
    loaded_ = !words_.empty();
    path_ = dicPath;
    return loaded_;
}

void SpellChecker::addWord(const std::string& word) {
    if (!word.empty()) added_.insert(lowered(word));
}

const std::string* SpellChecker::lookup(const std::string& word) const {
    auto it = words_.find(word);
    return it == words_.end() ? nullptr : &it->second;
}

bool SpellChecker::matchesCondition(const Rule& rule, const std::string& stem) const {
    const auto& sets = rule.cond.sets;
    size_t n = sets.size();
    if (n == 1 && sets[0] == ".") return true;
    // prefix conditions match the start of the stem, suffix conditions match its end
    if (stem.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char ch = rule.prefix ? stem[i] : stem[stem.size() - n + i];
        const std::string& set = sets[i];
        if (set == ".") continue;
        if (set[0] == '[') {
            bool negate = set.size() > 1 && set[1] == '^';
            bool in = set.find(ch, negate ? 2 : 1) != std::string::npos && set.find(ch, negate ? 2 : 1) < set.size() - 1;
            if (in == negate) return false;
        } else if (set[0] != ch) {
            return false;
        }
    }
    return true;
}

bool SpellChecker::hasBase(const std::string& word, char requiredFlag, char secondFlag) const {
    auto check = [&](const std::string& candidate) {
        const std::string* flags = lookup(candidate);
        if (!flags) return false;
        if (requiredFlag && flags->find(requiredFlag) == std::string::npos) return false;
        if (secondFlag && flags->find(secondFlag) == std::string::npos) return false;
        return true;
    };
    if (check(word)) return true;
    std::string low = lowered(word);
    if (low != word && check(low)) return true;
    return false;
}

bool SpellChecker::checkVariants(const std::string& word) const {
    if (lookup(word)) return true;
    for (const Rule& rule : rules_) {
        if (rule.prefix) {
            if (word.size() <= rule.add.size() || word.compare(0, rule.add.size(), rule.add) != 0) continue;
            std::string stem = rule.strip + word.substr(rule.add.size());
            if (!matchesCondition(rule, stem)) continue;
            if (hasBase(stem, rule.flag, 0)) return true;
            if (rule.cross) {
                for (const Rule& suf : rules_) {
                    if (suf.prefix || !suf.cross || word.size() <= suf.add.size() + rule.add.size()) continue;
                    if (stem.size() <= suf.add.size() || stem.compare(stem.size() - suf.add.size(), suf.add.size(), suf.add) != 0) continue;
                    std::string base = stem.substr(0, stem.size() - suf.add.size()) + suf.strip;
                    if (matchesCondition(suf, base) && hasBase(base, rule.flag, suf.flag)) return true;
                }
            }
        } else {
            if (word.size() <= rule.add.size() || word.compare(word.size() - rule.add.size(), rule.add.size(), rule.add) != 0) continue;
            std::string stem = word.substr(0, word.size() - rule.add.size()) + rule.strip;
            if (!matchesCondition(rule, stem)) continue;
            if (hasBase(stem, rule.flag, 0)) return true;
        }
    }
    return false;
}

bool SpellChecker::isCorrect(const std::string& word) const {
    if (!loaded_ || word.size() < 2) return true;
    if (std::any_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c); })) return true;
    if (added_.count(lowered(word))) return true;
    if (checkVariants(word)) return true;
    std::string low = lowered(word);
    if (low != word && (isCapitalized(word) || isAllUpper(word)) && checkVariants(low)) return true;
    if (low != word && isAllUpper(word) && isCapitalized(low.empty() ? low : std::string(1, static_cast<char>(std::toupper(low[0]))) + low.substr(1)) &&
        checkVariants(std::string(1, static_cast<char>(std::toupper(low[0]))) + low.substr(1))) return true;
    return false;
}

std::vector<std::string> SpellChecker::suggest(const std::string& word, size_t limit) const {
    std::vector<std::string> out;
    if (!loaded_ || word.empty()) return out;
    std::string w = lowered(word);
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz'";
    std::unordered_set<std::string> seen;
    auto offer = [&](const std::string& candidate) {
        if (candidate.size() < 2 || candidate == w || !seen.insert(candidate).second) return;
        if (isCorrect(candidate)) out.push_back(candidate);
    };
    for (size_t i = 0; i <= w.size() && out.size() < limit * 4; ++i) {
        if (i < w.size()) offer(w.substr(0, i) + w.substr(i + 1));                                  // deletion
        if (i + 1 < w.size()) { std::string t = w; std::swap(t[i], t[i + 1]); offer(t); }          // transposition
        for (char c : alphabet) {
            if (c == '\0') continue;
            if (i < w.size()) { std::string t = w; t[i] = c; offer(t); }                           // replacement
            offer(w.substr(0, i) + c + w.substr(i));                                               // insertion
        }
    }
    if (isCapitalized(word)) for (auto& s : out) if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (out.size() > limit) out.resize(limit);
    return out;
}
