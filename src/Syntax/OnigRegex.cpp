#include "Syntax/OnigRegex.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>

struct ClassSet {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::vector<std::string> named;  // posix / property names, optionally prefixed with ^ to negate
    std::vector<ClassSet> nested;
    std::vector<ClassSet> intersect;  // for [a&&b]: every operand must match
    bool negate = false;
};

struct OnigRegex::Node {
    enum Kind { Char, Any, Class, Seq, Alt, Group, NonCap, Repeat, Look, Atomic, Anchor, Backref, Empty } kind = Empty;
    uint32_t ch = 0;
    bool icase = false;
    ClassSet set;
    std::vector<int> kids;
    int index = 0;                 // group number, backref number, anchor kind
    int min = 0, max = -1, mode = 0;  // repeat: mode 0 greedy, 1 lazy
    bool behind = false, negative = false;  // lookaround
};

OnigRegex::~OnigRegex() = default;

namespace {

using Node = OnigRegex::Node;

enum AnchorKind { BOL, EOL, BOS, EOS, EOSNL, WORDB, NWORDB, ANCHOR_G };

bool isWordByte(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 128; }

// Decodes one UTF-8 character at pos; returns its length (at least 1).
size_t decodeUtf8(const std::string& s, size_t pos, uint32_t& cp) {
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80) { cp = c; return 1; }
    size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (len == 1 || pos + len > s.size()) { cp = c; return 1; }
    cp = c & (0xFF >> (len + 1));
    for (size_t i = 1; i < len; ++i) cp = (cp << 6) | (static_cast<unsigned char>(s[pos + i]) & 0x3F);
    return len;
}

bool namedClassHas(const std::string& rawName, uint32_t cp) {
    std::string name = rawName;
    bool neg = false;
    if (!name.empty() && name[0] == '^') { neg = true; name.erase(0, 1); }
    for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool ascii = cp < 128;
    int c = static_cast<int>(cp);
    bool r = false;
    if (name == "alpha" || name == "l" || name == "letter") r = ascii ? std::isalpha(c) != 0 : true;
    else if (name == "digit" || name == "d" || name == "n" || name == "nd") r = ascii && std::isdigit(c);
    else if (name == "alnum") r = ascii ? std::isalnum(c) != 0 : true;
    else if (name == "upper" || name == "lu") r = ascii && std::isupper(c);
    else if (name == "lower" || name == "ll") r = ascii && std::islower(c);
    else if (name == "space" || name == "s") r = ascii && std::isspace(c);
    else if (name == "blank") r = cp == ' ' || cp == '\t';
    else if (name == "punct" || name == "p") r = ascii && std::ispunct(c);
    else if (name == "word" || name == "w") r = ascii ? (std::isalnum(c) || c == '_') : true;
    else if (name == "xdigit" || name == "h") r = ascii && std::isxdigit(c);
    else if (name == "cntrl") r = ascii && std::iscntrl(c);
    else if (name == "graph") r = ascii ? (std::isgraph(c) != 0) : true;
    else if (name == "print") r = ascii ? (std::isprint(c) != 0) : true;
    else if (name == "ascii") r = ascii;
    return neg ? !r : r;
}

bool classMatches(const ClassSet& set, uint32_t cp, bool icase) {
    if (!set.intersect.empty()) {
        bool all = true;
        for (auto& operand : set.intersect) if (!classMatches(operand, cp, icase)) { all = false; break; }
        return set.negate ? !all : all;
    }
    bool hit = false;
    for (auto& [lo, hi] : set.ranges) {
        if (cp >= lo && cp <= hi) { hit = true; break; }
        if (icase && cp < 128) {
            uint32_t other = std::islower(static_cast<int>(cp)) ? static_cast<uint32_t>(std::toupper(static_cast<int>(cp))) : static_cast<uint32_t>(std::tolower(static_cast<int>(cp)));
            if (other >= lo && other <= hi) { hit = true; break; }
        }
    }
    if (!hit) for (auto& name : set.named) if (namedClassHas(name, cp)) { hit = true; break; }
    if (!hit) for (auto& inner : set.nested) if (classMatches(inner, cp, icase)) { hit = true; break; }
    return set.negate ? !hit : hit;
}

}  // namespace

struct Compiler {
    const std::string& pat;
    size_t i = 0;
    OnigRegex& re;
    std::string error;
    bool icase = false, extended = false;
    std::map<std::string, int> names;

    Compiler(const std::string& p, OnigRegex& r) : pat(p), re(r) {}

    int add(Node n) {
        re.nodes_.push_back(std::make_unique<Node>(std::move(n)));
        return static_cast<int>(re.nodes_.size()) - 1;
    }
    bool more() const { return i < pat.size(); }
    void fail(const std::string& why) { if (error.empty()) error = why; }

    int parseAlt() {
        std::vector<int> branches;
        branches.push_back(parseSeq());
        while (more() && pat[i] == '|') { ++i; branches.push_back(parseSeq()); }
        if (branches.size() == 1) return branches[0];
        Node n; n.kind = Node::Alt; n.kids = branches;
        return add(std::move(n));
    }

    void skipExtended() {
        while (extended && more()) {
            if (std::isspace(static_cast<unsigned char>(pat[i]))) { ++i; continue; }
            if (pat[i] == '#') { while (more() && pat[i] != '\n') ++i; continue; }
            break;
        }
    }

    int parseSeq() {
        std::vector<int> items;
        while (true) {
            skipExtended();
            if (!more() || pat[i] == '|' || pat[i] == ')') break;
            int atom = parseAtom();
            if (!error.empty()) return 0;
            if (atom < 0) continue;  // flag-only group
            atom = parseQuantifiers(atom);
            items.push_back(atom);
        }
        Node n; n.kind = Node::Seq; n.kids = items;
        return add(std::move(n));
    }

    int parseQuantifiers(int atom) {
        while (true) {
            skipExtended();
            if (!more()) return atom;
            int mn = 0, mx = -1;
            char c = pat[i];
            if (c == '*') { mn = 0; mx = -1; ++i; }
            else if (c == '+') { mn = 1; mx = -1; ++i; }
            else if (c == '?') { mn = 0; mx = 1; ++i; }
            else if (c == '{') {
                size_t j = i + 1;
                std::string a, b;
                bool comma = false;
                while (j < pat.size() && (std::isdigit(static_cast<unsigned char>(pat[j])) || pat[j] == ',')) {
                    if (pat[j] == ',') comma = true; else (comma ? b : a) += pat[j];
                    ++j;
                }
                if (j >= pat.size() || pat[j] != '}' || (a.empty() && b.empty())) return atom;  // a literal brace
                mn = a.empty() ? 0 : std::stoi(a);
                mx = comma ? (b.empty() ? -1 : std::stoi(b)) : mn;
                i = j + 1;
            } else return atom;
            int mode = 0;
            if (more() && pat[i] == '?') { mode = 1; ++i; }
            else if (more() && pat[i] == '+' && c != '{') { mode = 2; ++i; }
            Node rep; rep.kind = Node::Repeat; rep.kids = {atom}; rep.min = mn; rep.max = mx; rep.mode = mode == 1 ? 1 : 0;
            int node = add(std::move(rep));
            if (mode == 2) { Node at; at.kind = Node::Atomic; at.kids = {node}; node = add(std::move(at)); }
            atom = node;
        }
    }

    int charNode(uint32_t cp) { Node n; n.kind = Node::Char; n.ch = cp; n.icase = icase; return add(std::move(n)); }
    int classNode(ClassSet s) { Node n; n.kind = Node::Class; n.set = std::move(s); n.icase = icase; return add(std::move(n)); }
    int anchorNode(int kind) { Node n; n.kind = Node::Anchor; n.index = kind; return add(std::move(n)); }

    uint32_t readCodepoint() {
        uint32_t cp = 0;
        size_t len = decodeUtf8(pat, i, cp);
        i += len;
        return cp;
    }

    bool escapeClass(char e, ClassSet& out) {
        switch (e) {
            case 'd': out.named.push_back("digit"); return true;
            case 'D': out.named.push_back("^digit"); return true;
            case 'w': out.named.push_back("word"); return true;
            case 'W': out.named.push_back("^word"); return true;
            case 's': out.named.push_back("space"); return true;
            case 'S': out.named.push_back("^space"); return true;
            case 'h': out.named.push_back("xdigit"); return true;
            case 'H': out.named.push_back("^xdigit"); return true;
            default: return false;
        }
    }

    // After a backslash inside or outside a class: a single literal code point for simple escapes.
    uint32_t escapedChar(char e) {
        switch (e) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case 'f': return '\f';
            case 'v': return '\v';
            case 'a': return 7;
            case 'e': return 27;
            case 'x': case 'u': {
                int digits = e == 'u' ? 4 : 2;
                bool braces = more() && pat[i] == '{';
                if (braces) ++i;
                uint32_t v = 0;
                int n = 0;
                while (more() && std::isxdigit(static_cast<unsigned char>(pat[i])) && (braces || n < digits)) { v = v * 16 + static_cast<uint32_t>(std::stoi(std::string(1, pat[i]), nullptr, 16)); ++i; ++n; }
                if (braces && more() && pat[i] == '}') ++i;
                return v;
            }
            default: return static_cast<unsigned char>(e);
        }
    }

    void parsePropertyName(std::string& name, bool& negate) {
        // after \p or \P: {Name} or {^Name}
        negate = false;
        if (more() && pat[i] == '{') {
            ++i;
            if (more() && pat[i] == '^') { negate = true; ++i; }
            while (more() && pat[i] != '}') name += pat[i++];
            if (more()) ++i;
        }
    }

    ClassSet parseClass() {  // after '['
        ClassSet set;
        std::vector<ClassSet> operands;  // the finished parts of a && chain
        bool wholeNegate = false;
        auto closeClass = [&]() {
            if (operands.empty()) { set.negate = wholeNegate; return set; }
            operands.push_back(std::move(set));
            ClassSet joined;
            joined.negate = wholeNegate;
            joined.intersect = std::move(operands);
            return joined;
        };
        if (more() && pat[i] == '^') { wholeNegate = true; ++i; }
        bool first = true;
        while (more()) {
            char c = pat[i];
            if (c == ']' && !first) { ++i; return closeClass(); }
            first = false;
            if (c == '[') {
                if (i + 1 < pat.size() && pat[i + 1] == ':') {
                    size_t end = pat.find(":]", i + 2);
                    if (end != std::string::npos) {
                        std::string name = pat.substr(i + 2, end - i - 2);
                        set.named.push_back(name);
                        i = end + 2;
                        continue;
                    }
                }
                ++i;
                set.nested.push_back(parseClass());
                continue;
            }
            if (c == '&' && i + 1 < pat.size() && pat[i + 1] == '&') {
                i += 2;
                operands.push_back(std::move(set));
                set = ClassSet();
                continue;
            }
            uint32_t lo;
            if (c == '\\' && i + 1 < pat.size()) {
                char e = pat[i + 1];
                i += 2;
                if (escapeClass(e, set)) continue;
                if (e == 'p' || e == 'P') { std::string name; bool neg; parsePropertyName(name, neg); set.named.push_back((neg != (e == 'P') ? "^" : "") + name); continue; }
                lo = escapedChar(e);
            } else lo = readCodepoint();
            uint32_t hi = lo;
            if (i + 1 < pat.size() && pat[i] == '-' && pat[i + 1] != ']') {
                ++i;
                if (pat[i] == '\\' && i + 1 < pat.size()) { char e = pat[i + 1]; i += 2; hi = escapedChar(e); }
                else hi = readCodepoint();
            }
            set.ranges.emplace_back(lo, hi);
        }
        fail("unterminated character class");
        return closeClass();
    }

    int parseAtom() {
        char c = pat[i];
        if (c == '(') return parseGroup();
        if (c == '[') { ++i; return classNode(parseClass()); }
        if (c == '.') { ++i; Node n; n.kind = Node::Any; return add(std::move(n)); }
        if (c == '^') { ++i; return anchorNode(BOL); }
        if (c == '$') { ++i; return anchorNode(EOL); }
        if (c == '\\') {
            if (i + 1 >= pat.size()) { fail("trailing backslash"); return 0; }
            char e = pat[i + 1];
            i += 2;
            ClassSet cs;
            if (escapeClass(e, cs)) return classNode(cs);
            switch (e) {
                case 'b': return anchorNode(WORDB);
                case 'B': return anchorNode(NWORDB);
                case 'A': return anchorNode(BOS);
                case 'z': return anchorNode(EOS);
                case 'Z': return anchorNode(EOSNL);
                case 'G': return anchorNode(ANCHOR_G);
                case 'R': { ClassSet nl; nl.ranges.emplace_back('\n', '\r'); return classNode(nl); }
                case 'p': case 'P': { std::string name; bool neg; parsePropertyName(name, neg); ClassSet s; s.named.push_back((neg != (e == 'P') ? "^" : "") + name); return classNode(s); }
                case 'k': {
                    if (more() && (pat[i] == '<' || pat[i] == '\'')) {
                        char close = pat[i] == '<' ? '>' : '\'';
                        ++i;
                        std::string name;
                        while (more() && pat[i] != close) name += pat[i++];
                        if (more()) ++i;
                        Node n; n.kind = Node::Backref; n.icase = icase;
                        if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) n.index = std::stoi(name);
                        else n.index = names.count(name) ? names[name] : 0;
                        return add(std::move(n));
                    }
                    return charNode('k');
                }
                default:
                    if (e >= '1' && e <= '9') { Node n; n.kind = Node::Backref; n.icase = icase; n.index = e - '0'; return add(std::move(n)); }
                    return charNode(escapedChar(e));
            }
        }
        return charNode(readCodepoint());
    }

    int parseGroup() {  // at '('
        ++i;
        Node n;
        if (more() && pat[i] == '?') {
            ++i;
            if (!more()) { fail("bad group"); return 0; }
            char t = pat[i];
            if (t == ':') { ++i; n.kind = Node::NonCap; }
            else if (t == '>') { ++i; n.kind = Node::Atomic; }
            else if (t == '=' || t == '!') { ++i; n.kind = Node::Look; n.negative = t == '!'; }
            else if (t == '<' && i + 1 < pat.size() && (pat[i + 1] == '=' || pat[i + 1] == '!')) { n.kind = Node::Look; n.behind = true; n.negative = pat[i + 1] == '!'; i += 2; }
            else if (t == '<' || t == '\'' || t == 'P') {
                if (t == 'P') ++i;
                char close = pat[i] == '\'' ? '\'' : '>';
                ++i;
                std::string name;
                while (more() && pat[i] != close) name += pat[i++];
                if (more()) ++i;
                n.kind = Node::Group; n.index = ++re.groupCount_;
                names[name] = n.index;
            } else if (t == '#') {
                while (more() && pat[i] != ')') ++i;
                if (more()) ++i;
                return -1;
            } else {
                // inline flags: (?i) (?x) (?-i) (?i:...)
                bool on = true;
                bool oldIcase = icase, oldExt = extended;
                while (more() && pat[i] != ')' && pat[i] != ':') {
                    char f = pat[i++];
                    if (f == '-') on = false;
                    else if (f == 'i') icase = on;
                    else if (f == 'x') extended = on;
                }
                if (more() && pat[i] == ')') { ++i; return -1; }  // applies to the rest of the enclosing group
                if (more() && pat[i] == ':') ++i;
                n.kind = Node::NonCap;
                int body = parseAlt();
                if (more() && pat[i] == ')') ++i; else fail("missing )");
                icase = oldIcase; extended = oldExt;
                n.kids = {body};
                return add(std::move(n));
            }
        } else {
            n.kind = Node::Group; n.index = ++re.groupCount_;
        }
        bool savedIcase = icase, savedExt = extended;
        int body = parseAlt();
        if (more() && pat[i] == ')') ++i; else fail("missing )");
        icase = savedIcase; extended = savedExt;
        n.kids = {body};
        return add(std::move(n));
    }
};

std::shared_ptr<OnigRegex> OnigRegex::compile(const std::string& pattern, std::string& error) {
    std::shared_ptr<OnigRegex> re(new OnigRegex());
    Compiler c(pattern, *re);
    int root = c.parseAlt();
    if (c.error.empty() && c.i < pattern.size()) c.fail("unmatched )");
    if (!c.error.empty()) { error = c.error; return nullptr; }
    re->root_ = root;
    return re;
}

namespace {

struct Matcher {
    const OnigRegex& re;
    const std::vector<std::unique_ptr<Node>>& nodes;
    const std::string& text;
    size_t anchor;
    std::vector<std::pair<long, long>> caps;
    long steps = 0;

    using Cont = std::function<bool(size_t)>;

    bool seq(const std::vector<int>& kids, size_t idx, size_t pos, const Cont& k) {
        if (idx == kids.size()) return k(pos);
        return run(kids[idx], pos, [&](size_t p) { return seq(kids, idx + 1, p, k); });
    }

    bool charEq(uint32_t a, uint32_t b, bool icase) {
        if (a == b) return true;
        if (icase && a < 128 && b < 128) return std::tolower(static_cast<int>(a)) == std::tolower(static_cast<int>(b));
        return false;
    }

    bool run(int id, size_t pos, const Cont& k) {
        if (++steps > 2000000) return false;  // runaway backtracking
        const Node& n = *nodes[static_cast<size_t>(id)];
        switch (n.kind) {
            case Node::Empty: return k(pos);
            case Node::Char: {
                if (pos >= text.size()) return false;
                uint32_t cp;
                size_t len = decodeUtf8(text, pos, cp);
                return charEq(cp, n.ch, n.icase) && k(pos + len);
            }
            case Node::Any: {
                if (pos >= text.size() || text[pos] == '\n') return false;
                uint32_t cp;
                return k(pos + decodeUtf8(text, pos, cp));
            }
            case Node::Class: {
                if (pos >= text.size()) return false;
                uint32_t cp;
                size_t len = decodeUtf8(text, pos, cp);
                return classMatches(n.set, cp, n.icase) && k(pos + len);
            }
            case Node::Seq: return seq(n.kids, 0, pos, k);
            case Node::Alt:
                for (int kid : n.kids) if (run(kid, pos, k)) return true;
                return false;
            case Node::NonCap: return run(n.kids[0], pos, k);
            case Node::Group: {
                size_t idx = static_cast<size_t>(n.index);
                return run(n.kids[0], pos, [&](size_t p) {
                    auto old = caps[idx];
                    caps[idx] = {static_cast<long>(pos), static_cast<long>(p)};
                    if (k(p)) return true;
                    caps[idx] = old;
                    return false;
                });
            }
            case Node::Atomic: {
                size_t endPos = 0;
                if (!run(n.kids[0], pos, [&](size_t p) { endPos = p; return true; })) return false;
                return k(endPos);
            }
            case Node::Look: {
                bool ok = false;
                if (!n.behind) {
                    ok = run(n.kids[0], pos, [](size_t) { return true; });
                } else {
                    size_t lowest = pos > 255 ? pos - 255 : 0;
                    for (size_t s = pos + 1; s-- > lowest && !ok;) ok = run(n.kids[0], s, [&](size_t p) { return p == pos; });
                }
                if (ok == n.negative) return false;
                return k(pos);
            }
            case Node::Anchor: {
                bool ok = false;
                switch (n.index) {
                    case BOL: ok = pos == 0 || text[pos - 1] == '\n'; break;
                    case EOL: ok = pos == text.size() || text[pos] == '\n'; break;
                    case BOS: ok = pos == 0; break;
                    case EOS: ok = pos == text.size(); break;
                    case EOSNL: ok = pos == text.size() || (pos + 1 == text.size() && text[pos] == '\n'); break;
                    case WORDB: case NWORDB: {
                        bool before = pos > 0 && isWordByte(static_cast<unsigned char>(text[pos - 1]));
                        bool after = pos < text.size() && isWordByte(static_cast<unsigned char>(text[pos]));
                        ok = (before != after) == (n.index == WORDB);
                        break;
                    }
                    case ANCHOR_G: ok = pos == anchor; break;
                }
                return ok && k(pos);
            }
            case Node::Backref: {
                size_t idx = static_cast<size_t>(n.index);
                if (idx >= caps.size() || caps[idx].first < 0) return false;
                size_t len = static_cast<size_t>(caps[idx].second - caps[idx].first);
                if (pos + len > text.size()) return false;
                for (size_t j = 0; j < len; ++j)
                    if (!charEq(static_cast<unsigned char>(text[static_cast<size_t>(caps[idx].first) + j]), static_cast<unsigned char>(text[pos + j]), n.icase)) return false;
                return k(pos + len);
            }
            case Node::Repeat: return repeat(n, 0, pos, k);
        }
        return false;
    }

    bool repeat(const Node& n, int count, size_t pos, const Cont& k) {
        bool canMore = n.max < 0 || count < n.max;
        auto more = [&]() {
            return run(n.kids[0], pos, [&](size_t p) {
                if (p == pos && count >= n.min) return false;  // an empty iteration cannot make progress
                return repeat(n, count + 1, p, k);
            });
        };
        if (count < n.min) return canMore && more();
        if (n.mode == 1) return k(pos) || (canMore && more());
        return (canMore && more()) || k(pos);
    }
};

}  // namespace

bool OnigRegex::search(const std::string& text, size_t from, size_t anchor, Match& out) const {
    for (size_t start = from; start <= text.size(); ++start) {
        // never begin in the middle of a multi-byte character
        if (start < text.size() && start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) continue;
        Matcher m{*this, nodes_, text, anchor, std::vector<std::pair<long, long>>(static_cast<size_t>(groupCount_) + 1, {-1, -1}), 0};
        size_t endPos = 0;
        if (m.run(root_, start, [&](size_t p) { endPos = p; return true; })) {
            m.caps[0] = {static_cast<long>(start), static_cast<long>(endPos)};
            out.groups = std::move(m.caps);
            return true;
        }
    }
    return false;
}
