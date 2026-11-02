#include "Syntax/SublimeSyntax.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <zlib.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

// ── A reader for the YAML subset that .sublime-syntax files use ─────────────────────────────────────────

namespace {

struct YLine { int indent; std::string text; };

std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Removes a trailing comment, leaving # inside quotes alone.
std::string stripComment(const std::string& line) {
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote) {
            if (quote == '"' && c == '\\') { ++i; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { if (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == '[' || line[i - 1] == '{' || line[i - 1] == ',' || line[i - 1] == ':' || line[i - 1] == '-') quote = c; }
        else if (c == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1])))) return line.substr(0, i);
    }
    return line;
}

class Yaml {
public:
    explicit Yaml(const std::string& text) {
        std::istringstream in(text);
        std::string raw;
        while (std::getline(in, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            raw_.push_back(raw);
        }
    }

    json parse() {
        build();
        size_t i = 0;
        json v = block(i, 0);
        return v;
    }

private:
    std::vector<std::string> raw_;
    std::vector<YLine> lines_;
    std::vector<size_t> rawIndex_;

    void build() {
        for (size_t r = 0; r < raw_.size(); ++r) {
            const std::string& line = raw_[r];
            std::string t = stripComment(line);
            if (trimmed(t).empty()) continue;
            if (trimmed(t).rfind("%", 0) == 0 || trimmed(t) == "---") continue;
            int indent = 0;
            while (indent < static_cast<int>(t.size()) && t[static_cast<size_t>(indent)] == ' ') ++indent;
            lines_.push_back({indent, trimmed(t.substr(static_cast<size_t>(indent)))});
            rawIndex_.push_back(r);
        }
    }

    static json scalar(const std::string& text) {
        std::string t = trimmed(text);
        if (t.size() >= 2 && t.front() == '\'' && t.back() == '\'') {
            std::string out;
            for (size_t i = 1; i + 1 < t.size(); ++i) { if (t[i] == '\'' && i + 2 < t.size() && t[i + 1] == '\'') ++i; out += t[i]; }
            return out;
        }
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            std::string out;
            for (size_t i = 1; i + 1 < t.size(); ++i) {
                if (t[i] == '\\' && i + 2 < t.size()) {
                    char e = t[++i];
                    switch (e) { case 'n': out += '\n'; break; case 't': out += '\t'; break; case '"': out += '"'; break; case '\\': out += '\\'; break; case 'e': out += '\x1b'; break; default: out += '\\'; out += e; }
                } else out += t[i];
            }
            return out;
        }
        if (t == "true") return true;
        if (t == "false") return false;
        if (t == "~" || t == "null") return nullptr;
        if (!t.empty() && (std::isdigit(static_cast<unsigned char>(t[0])) || (t[0] == '-' && t.size() > 1 && std::isdigit(static_cast<unsigned char>(t[1]))))) {
            bool numeric = true;
            for (size_t i = (t[0] == '-' ? 1 : 0); i < t.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(t[i]))) numeric = false;
            if (numeric) return std::stoll(t);
        }
        return t;
    }

    // [a, b, 'c d'] and {k: v}
    json flow(const std::string& s, size_t& i) {
        auto skip = [&] { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
        skip();
        if (i >= s.size()) return nullptr;
        if (s[i] == '[') {
            ++i;
            json arr = json::array();
            while (true) {
                skip();
                if (i >= s.size()) break;
                if (s[i] == ']') { ++i; break; }
                if (s[i] == ',') { ++i; continue; }
                arr.push_back(flow(s, i));
            }
            return arr;
        }
        if (s[i] == '{') {
            ++i;
            json obj = json::object();
            while (true) {
                skip();
                if (i >= s.size()) break;
                if (s[i] == '}') { ++i; break; }
                if (s[i] == ',') { ++i; continue; }
                json key = flowScalar(s, i, true);
                skip();
                if (i < s.size() && s[i] == ':') ++i;
                json value = flow(s, i);
                obj[key.is_string() ? key.get<std::string>() : key.dump()] = value;
            }
            return obj;
        }
        return flowScalar(s, i, false);
    }

    json flowScalar(const std::string& s, size_t& i, bool isKey) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t start = i;
        if (i < s.size() && (s[i] == '\'' || s[i] == '"')) {
            char q = s[i++];
            while (i < s.size() && s[i] != q) { if (q == '"' && s[i] == '\\') ++i; if (q == '\'' && s[i] == '\'' && i + 1 < s.size() && s[i + 1] == '\'') ++i; ++i; }
            if (i < s.size()) ++i;
            return scalar(s.substr(start, i - start));
        }
        while (i < s.size() && s[i] != ',' && s[i] != ']' && s[i] != '}' && !(isKey && s[i] == ':' ) && !(s[i] == ':' && (i + 1 >= s.size() || s[i + 1] == ' '))) ++i;
        return scalar(s.substr(start, i - start));
    }

    static size_t colonAt(const std::string& t) {
        char quote = 0;
        for (size_t i = 0; i < t.size(); ++i) {
            char c = t[i];
            if (quote) { if (quote == '"' && c == '\\') { ++i; continue; } if (c == quote) quote = 0; continue; }
            if (c == '"' || c == '\'') { if (i == 0) quote = c; continue; }
            if (c == ':' && (i + 1 == t.size() || t[i + 1] == ' ')) return i;
        }
        return std::string::npos;
    }

    json valueText(const std::string& text) {
        std::string t = trimmed(text);
        if (!t.empty() && (t[0] == '[' || t[0] == '{')) { size_t i = 0; return flow(t, i); }
        return scalar(t);
    }

    json blockScalar(size_t& i, int parentIndent, bool folded) {
        std::string out;
        size_t lastRaw = rawIndex_[i - 1];
        int indent = -1;
        for (size_t r = lastRaw + 1; r < raw_.size(); ++r) {
            const std::string& line = raw_[r];
            if (trimmed(line).empty()) { out += "\n"; continue; }
            int ind = 0;
            while (ind < static_cast<int>(line.size()) && line[static_cast<size_t>(ind)] == ' ') ++ind;
            if (ind <= parentIndent) break;
            if (indent < 0) indent = ind;
            out += line.substr(static_cast<size_t>(std::min(ind, indent))) + (folded ? " " : "\n");
        }
        // consume the lines that belonged to the scalar
        while (i < lines_.size() && lines_[i].indent > parentIndent) ++i;
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        return out;
    }

    json block(size_t& i, int indent) {
        if (i >= lines_.size()) return nullptr;
        indent = lines_[i].indent;
        if (lines_[i].text.rfind("- ", 0) == 0 || lines_[i].text == "-") return sequence(i, indent);
        if (colonAt(lines_[i].text) == std::string::npos) {
            // a plain scalar written over several lines
            std::string joined = lines_[i].text;
            ++i;
            while (i < lines_.size() && lines_[i].indent >= indent && colonAt(lines_[i].text) == std::string::npos && lines_[i].text.rfind("- ", 0) != 0) { joined += " " + lines_[i].text; ++i; }
            return valueText(joined);
        }
        return mapping(i, indent);
    }

    json sequence(size_t& i, int indent) {
        json arr = json::array();
        while (i < lines_.size() && lines_[i].indent == indent && (lines_[i].text.rfind("- ", 0) == 0 || lines_[i].text == "-")) {
            std::string rest = lines_[i].text == "-" ? "" : trimmed(lines_[i].text.substr(2));
            if (rest.empty()) {
                ++i;
                if (i < lines_.size() && lines_[i].indent > indent) arr.push_back(block(i, lines_[i].indent));
                else arr.push_back(nullptr);
            } else if (colonAt(rest) != std::string::npos && rest[0] != '[' && rest[0] != '{' && rest[0] != '\'' && rest[0] != '"') {
                // a mapping starting on the dash line: re-indent the line so it reads as the first key of the map
                int inner = indent + 2;
                lines_[i].indent = inner;
                lines_[i].text = rest;
                arr.push_back(mapping(i, inner));
            } else {
                arr.push_back(valueText(rest));
                ++i;
            }
        }
        return arr;
    }

    json mapping(size_t& i, int indent) {
        json obj = json::object();
        while (i < lines_.size() && lines_[i].indent == indent && !(lines_[i].text.rfind("- ", 0) == 0)) {
            const std::string text = lines_[i].text;
            size_t colon = colonAt(text);
            if (colon == std::string::npos) { ++i; continue; }
            std::string key = trimmed(text.substr(0, colon));
            json keyValue = scalar(key);
            std::string keyText = keyValue.is_string() ? keyValue.get<std::string>() : (keyValue.is_null() ? key : keyValue.dump());
            std::string rest = trimmed(text.substr(colon + 1));
            ++i;
            if (rest.empty()) {
                if (i < lines_.size() && lines_[i].indent > indent) obj[keyText] = block(i, lines_[i].indent);
                else if (i < lines_.size() && lines_[i].indent == indent && lines_[i].text.rfind("- ", 0) == 0) obj[keyText] = sequence(i, indent);
                else obj[keyText] = nullptr;
            } else if (rest == "|" || rest == ">" || rest == "|-" || rest == ">-" || rest == "|+" || rest == ">+") {
                obj[keyText] = blockScalar(i, indent, rest[0] == '>');
            } else {
                // plain scalars may continue on following, more indented lines
                if (rest[0] != '"' && rest[0] != '\'' && rest[0] != '[' && rest[0] != '{')
                    while (i < lines_.size() && lines_[i].indent > indent && colonAt(lines_[i].text) == std::string::npos && lines_[i].text.rfind("- ", 0) != 0) { rest += " " + lines_[i].text; ++i; }
                obj[keyText] = valueText(rest);
            }
        }
        return obj;
    }
};

}  // namespace

// ── The interpreter ──────────────────────────────────────────────────────────────────────────────────────

struct Context;

struct Pattern {
    enum Kind { Match, Include, Embed } kind = Match;
    std::string source;                       // match regex text (variables expanded)
    std::string rawSource, rawEscape;         // before variable expansion, for syntaxes that extend another
    const SublimeSyntax::Impl* owner = nullptr;  // the syntax this pattern was written in: context names in push/set resolve there
    mutable std::shared_ptr<OnigRegex> regex;
    mutable bool compileTried = false;
    std::vector<std::string> scope;
    std::map<int, std::vector<std::string>> captures;
    enum Action { None, Push, Pop, Set, Branch, Fail } action = None;
    std::string branchPoint, failName;
    std::vector<std::string> branchTargets;
    int popCount = 1;
    std::vector<std::string> targets;         // context names, or scope:... references
    std::vector<std::shared_ptr<Context>> inlineTargets;
    std::vector<std::shared_ptr<Context>> withPrototype;  // contexts holding the with_prototype patterns
    std::string includeName;
    std::string embedScope, escape;
    std::vector<std::string> embedScopeNames;
    std::map<int, std::vector<std::string>> escapeCaptures;
};

struct Context {
    std::string name;
    std::vector<std::string> metaScope, metaContentScope;
    bool includePrototype = true;
    bool clearScopes = false;
    bool metaPrepend = false, metaAppend = false;
    bool hasMetaScope = false, hasMetaContentScope = false;
    std::vector<Pattern> patterns;
    const SublimeSyntax::Impl* owner = nullptr;
};

struct SublimeSyntax::Impl {
    std::map<std::string, std::vector<std::string>> unusedVars;
    std::map<std::string, std::string> variables;
    std::map<std::string, std::shared_ptr<Context>> contexts;
    std::vector<std::shared_ptr<Context>> anonymous;
    std::vector<std::string> extends;
    std::string sourcePath;
    bool resolved = false;
    std::string baseScope;
    int anonCounter = 0;
};

SublimeSyntax::SublimeSyntax() : impl_(new Impl()) {}
SublimeSyntax::~SublimeSyntax() = default;

namespace {

std::vector<std::string> splitScopes(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string expandVariables(const std::string& text, const std::map<std::string, std::string>& vars, int depth = 0) {
    if (depth > 8) return text;
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "{{") == 0) {
            size_t end = text.find("}}", i + 2);
            if (end != std::string::npos) {
                std::string name = text.substr(i + 2, end - i - 2);
                auto it = vars.find(name);
                if (it != vars.end()) { out += expandVariables(it->second, vars, depth + 1); i = end + 2; continue; }
            }
        }
        out += text[i++];
    }
    return out;
}

struct Loader {
    SublimeSyntax::Impl& impl;

    std::shared_ptr<Context> makeContext(const std::string& name, const json& items) {
        auto ctx = std::make_shared<Context>();
        ctx->name = name;
        ctx->owner = &impl;
        if (!items.is_array()) return ctx;
        for (auto& item : items) {
            if (!item.is_object()) continue;
            if (item.contains("meta_scope")) { ctx->metaScope = splitScopes(item["meta_scope"].get<std::string>()); ctx->hasMetaScope = true; }
            if (item.contains("meta_content_scope")) { ctx->metaContentScope = splitScopes(item["meta_content_scope"].get<std::string>()); ctx->hasMetaContentScope = true; }
            if (item.contains("meta_prepend")) ctx->metaPrepend = true;
            if (item.contains("meta_append")) ctx->metaAppend = true;
            if (item.contains("meta_include_prototype")) ctx->includePrototype = item["meta_include_prototype"].is_boolean() ? item["meta_include_prototype"].get<bool>() : true;
            if (item.contains("clear_scopes")) ctx->clearScopes = true;
            if (item.contains("match") || item.contains("include") || item.contains("embed")) ctx->patterns.push_back(makePattern(item));
        }
        return ctx;
    }

    std::map<int, std::vector<std::string>> readCaptures(const json& node) {
        std::map<int, std::vector<std::string>> out;
        if (!node.is_object()) return out;
        for (auto it = node.begin(); it != node.end(); ++it) {
            try { out[std::stoi(it.key())] = splitScopes(it.value().is_string() ? it.value().get<std::string>() : std::string()); } catch (...) {}
        }
        return out;
    }

    void readTargets(const json& v, Pattern& p) {
        if (v.is_string()) { p.targets.push_back(v.get<std::string>()); return; }
        if (!v.is_array()) return;
        bool inlineContext = !v.empty() && v[0].is_object();
        if (inlineContext) {
            p.inlineTargets.push_back(makeContext("__inline" + std::to_string(impl.anonCounter++), v));
            return;
        }
        for (auto& t : v) {
            if (t.is_string()) p.targets.push_back(t.get<std::string>());
            else if (t.is_array()) p.inlineTargets.push_back(makeContext("__inline" + std::to_string(impl.anonCounter++), t));
        }
    }

    Pattern makePattern(const json& item) {
        Pattern p;
        p.owner = &impl;
        if (item.contains("include") && item["include"].is_string()) {
            p.kind = Pattern::Include;
            p.includeName = item["include"].get<std::string>();
            return p;
        }
        if (item.contains("embed")) {
            p.kind = Pattern::Embed;
            p.embedScope = item["embed"].is_string() ? item["embed"].get<std::string>() : std::string();
            if (item.contains("embed_scope") && item["embed_scope"].is_string()) p.embedScopeNames = splitScopes(item["embed_scope"].get<std::string>());
            if (item.contains("escape") && item["escape"].is_string()) { p.rawEscape = item["escape"].get<std::string>(); p.escape = expandVariables(p.rawEscape, impl.variables); }
            p.escapeCaptures = readCaptures(item.value("escape_captures", json()));
            p.rawSource = item.contains("match") ? item["match"].get<std::string>() : std::string();
            p.source = expandVariables(p.rawSource, impl.variables);
            if (item.contains("scope") && item["scope"].is_string()) p.scope = splitScopes(item["scope"].get<std::string>());
            p.captures = readCaptures(item.value("captures", json()));
            if (item.contains("pop")) {
                const json& pop = item["pop"];
                if (pop.is_number()) { p.action = Pattern::Pop; p.popCount = std::max(1, pop.get<int>()); }
                else if (pop.is_boolean() && pop.get<bool>()) p.action = Pattern::Pop;
            }
            return p;
        }
        p.rawSource = item["match"].get<std::string>();
        p.source = expandVariables(p.rawSource, impl.variables);
        if (item.contains("scope") && item["scope"].is_string()) p.scope = splitScopes(item["scope"].get<std::string>());
        p.captures = readCaptures(item.value("captures", json()));
        if (item.contains("branch_point") && item["branch_point"].is_string() && item.contains("branch") && item["branch"].is_array()) {
            p.action = Pattern::Branch;
            p.branchPoint = item["branch_point"].get<std::string>();
            for (auto& b : item["branch"]) if (b.is_string()) p.branchTargets.push_back(b.get<std::string>());
        }
        else if (item.contains("fail") && item["fail"].is_string()) { p.action = Pattern::Fail; p.failName = item["fail"].get<std::string>(); }
        else if (item.contains("push")) { p.action = Pattern::Push; readTargets(item["push"], p); }
        else if (item.contains("set")) { p.action = Pattern::Set; readTargets(item["set"], p); }
        else if (item.contains("pop")) {
            const json& pop = item["pop"];
            if (pop.is_number()) { p.action = Pattern::Pop; p.popCount = std::max(1, pop.get<int>()); }
            else if (pop.is_boolean() && pop.get<bool>()) p.action = Pattern::Pop;
        }
        if (item.contains("with_prototype") && item["with_prototype"].is_array())
            p.withPrototype.push_back(makeContext("__with_prototype", item["with_prototype"]));
        return p;
    }
};

}  // namespace

std::shared_ptr<SublimeSyntax> SublimeSyntax::parse(const std::string& yamlText, std::string& error) {
    json doc;
    try { doc = Yaml(yamlText).parse(); } catch (const std::exception& e) { error = e.what(); return nullptr; }
    if (!doc.is_object()) { error = "not a mapping"; return nullptr; }
    bool extending = doc.contains("extends");
    if (!extending && (!doc.contains("contexts") || !doc["contexts"].is_object())) { error = "no contexts"; return nullptr; }
    std::shared_ptr<SublimeSyntax> syn(new SublimeSyntax());
    syn->name_ = doc.value("name", std::string());
    syn->scope_ = doc.value("scope", std::string());
    syn->impl_->baseScope = syn->scope_;
    if (doc.contains("first_line_match") && doc["first_line_match"].is_string()) syn->firstLine_ = doc["first_line_match"].get<std::string>();
    if (doc.contains("file_extensions") && doc["file_extensions"].is_array())
        for (auto& e : doc["file_extensions"]) if (e.is_string()) syn->extensions_.push_back(e.get<std::string>());
    if (doc.contains("variables") && doc["variables"].is_object())
        for (auto it = doc["variables"].begin(); it != doc["variables"].end(); ++it)
            if (it.value().is_string()) syn->impl_->variables[it.key()] = it.value().get<std::string>();
    if (extending) {
        if (doc["extends"].is_string()) syn->impl_->extends.push_back(doc["extends"].get<std::string>());
        else if (doc["extends"].is_array()) for (auto& e : doc["extends"]) if (e.is_string()) syn->impl_->extends.push_back(e.get<std::string>());
    }
    Loader loader{*syn->impl_};
    if (doc.contains("contexts") && doc["contexts"].is_object())
        for (auto it = doc["contexts"].begin(); it != doc["contexts"].end(); ++it)
            syn->impl_->contexts[it.key()] = loader.makeContext(it.key(), it.value());
    syn->impl_->resolved = !extending;
    if (std::getenv("MORENO_SYNTAX_DEBUG")) std::fprintf(stderr, "contexts parsed: %zu (else-pop %d)", syn->impl_->contexts.size(), static_cast<int>(syn->impl_->contexts.count("else-pop")));
    if (std::getenv("MORENO_SYNTAX_DEBUG")) std::fputc(10, stderr);
    if (std::getenv("MORENO_SYNTAX_LIST")) for (auto& kv : syn->impl_->contexts) { std::fputs(kv.first.c_str(), stdout); std::fputc(10, stdout); }
    if (!extending && !syn->impl_->contexts.count("main")) { error = "no main context"; return nullptr; }
    return syn;
}

// ── Tokenizing ───────────────────────────────────────────────────────────────────────────────────────────

namespace {

struct Frame {
    const Context* ctx = nullptr;
    const std::vector<std::shared_ptr<Context>>* withPrototype = nullptr;
    std::shared_ptr<Context> temp;      // an embed context built for this frame
    size_t metaCount = 0, contentCount = 0;
};

struct Resolved { const Context* ctx = nullptr; std::shared_ptr<Context> keep; };

struct Tokenizer {
    const SublimeSyntax::Impl& root;
    const SyntaxLibrary* library;
    std::vector<Frame> stack;
    std::vector<std::string> scopes;
    std::vector<ScopeSpan> spans;
    std::deque<std::shared_ptr<Context>> temps;
    std::map<const SublimeSyntax::Impl*, std::shared_ptr<SublimeSyntax>> libraryHold;

    // Backtracking for branch/fail: the state before a branch pattern ran, so a failing alternative can be undone.
    struct Snapshot {
        std::string name;
        const Pattern* pattern = nullptr;
        size_t lineStart = 0, pos = 0;
        std::vector<Frame> stack;
        std::vector<std::string> scopes;
        size_t spanCount = 0, lastSpanLength = 0;
        size_t nextAlternative = 1;
    };
    std::vector<Snapshot> snapshots;
    bool jumpPending = false;
    size_t jumpLine = 0, jumpPos = 0;
    const Pattern* altPattern = nullptr;
    size_t altLine = 0, altPos = 0, altIndex = 0;

    Tokenizer(const SublimeSyntax::Impl& r, const SyntaxLibrary* lib) : root(r), library(lib) {}

    const Context* find(const SublimeSyntax::Impl& owner, const std::string& name, const SublimeSyntax::Impl** foundOwner) const {
        if (name.rfind("scope:", 0) == 0 && library) {
            std::string rest = name.substr(6);
            std::string ctxName = "main";
            size_t hash = rest.find('#');
            if (hash != std::string::npos) { ctxName = rest.substr(hash + 1); rest = rest.substr(0, hash); }
            auto other = library->byScope(rest);
            if (!other) return nullptr;
            const SublimeSyntax::Impl* oi = other->impl();
            auto it = oi->contexts.find(ctxName);
            if (it == oi->contexts.end()) return nullptr;
            *foundOwner = oi;
            return it->second.get();
        }
        auto it = owner.contexts.find(name);
        if (it == owner.contexts.end()) return nullptr;
        *foundOwner = &owner;
        return it->second.get();
    }

    // The patterns that apply in a frame, includes expanded, prototype and with_prototype first.
    void collect(const Context& ctx, const SublimeSyntax::Impl& owner, std::vector<const Pattern*>& out, int depth, bool top) const {
        if (depth > 12) return;
        if (top && ctx.includePrototype) {
            auto proto = owner.contexts.find("prototype");
            if (proto != owner.contexts.end() && proto->second.get() != &ctx) collect(*proto->second, owner, out, depth + 1, false);
        }
        for (const Pattern& p : ctx.patterns) {
            if (p.kind != Pattern::Include) { out.push_back(&p); continue; }
            const SublimeSyntax::Impl* foundOwner = nullptr;
            const Context* inc = find(owner, p.includeName, &foundOwner);
            if (inc) collect(*inc, *foundOwner, out, depth + 1, false);
        }
    }

    void currentScopes(std::string& out, size_t drop = 0) const {
        out.clear();
        size_t n = scopes.size() > drop ? scopes.size() - drop : 0;
        for (size_t i = 0; i < n; ++i) { if (i) out += ' '; out += scopes[i]; }
    }

    void emit(size_t start, size_t end, const std::string& scope) {
        if (end <= start) return;
        if (!spans.empty() && spans.back().start + spans.back().length == start && spans.back().scope == scope) { spans.back().length += end - start; return; }
        spans.push_back({start, end - start, scope});
    }

    void pushFrame(const Context* ctx, const std::vector<std::shared_ptr<Context>>* proto, std::shared_ptr<Context> temp) {
        Frame f;
        f.ctx = ctx; f.withPrototype = proto; f.temp = std::move(temp);
        for (auto& s : ctx->metaScope) { scopes.push_back(s); ++f.metaCount; }
        for (auto& s : ctx->metaContentScope) { scopes.push_back(s); ++f.contentCount; }
        stack.push_back(std::move(f));
    }

    void popFrame() {
        if (stack.size() <= 1) return;
        Frame& f = stack.back();
        size_t n = f.metaCount + f.contentCount;
        scopes.resize(scopes.size() >= n ? scopes.size() - n : 0);
        stack.pop_back();
    }

    std::vector<ScopeSpan> run(const std::string& text) {
        const Context* mainCtx = root.contexts.at("main").get();
        scopes.clear();
        if (!root.baseScope.empty()) scopes.push_back(root.baseScope);
        pushFrame(mainCtx, nullptr, nullptr);
        size_t lineStart = 0, startPos = 0;
        long rewinds = 0;
        while (lineStart < text.size()) {
            size_t nl = text.find('\n', lineStart);
            size_t lineEnd = nl == std::string::npos ? text.size() : nl + 1;
            tokenizeLine(text, lineStart, lineEnd, startPos);
            startPos = 0;
            if (jumpPending) {
                jumpPending = false;
                lineStart = jumpLine;
                startPos = jumpPos;
                if (++rewinds > 20000) break;  // a grammar that keeps failing; give up on the rest
                continue;
            }
            lineStart = lineEnd;
        }
        return std::move(spans);
    }

    void tokenizeLine(const std::string& full, size_t lineStart, size_t lineEnd, size_t startPos) {
        std::string line = full.substr(lineStart, lineEnd - lineStart);
        size_t pos = startPos, anchor = startPos;
        int guard = 0;
        std::string scopeText;
        while (pos < line.size() && ++guard < 4000) {
            Frame& frame = stack.back();
            const SublimeSyntax::Impl* owner = frame.ctx->owner;
            std::vector<const Pattern*> candidates;
            if (frame.withPrototype) for (auto& wp : *frame.withPrototype) collect(*wp, *owner, candidates, 0, false);
            collect(*frame.ctx, *owner, candidates, 0, true);
            // an embed's escape is live while anything above the embed is on the stack, and wins ties
            std::vector<std::pair<const Pattern*, size_t>> escapes;
            for (size_t j = stack.size(); j-- > 0;)
                if (stack[j].temp && stack[j].temp->name == "__embed" && !stack[j].temp->patterns.empty()) escapes.emplace_back(&stack[j].temp->patterns[0], j);
            for (size_t e = escapes.size(); e-- > 0;) candidates.insert(candidates.begin(), escapes[e].first);

            const Pattern* best = nullptr;
            OnigRegex::Match bestMatch;
            long bestStart = -1;
            for (const Pattern* p : candidates) {
                const std::string& src = p->kind == Pattern::Embed ? p->source : p->source;
                if (!p->compileTried) {
                    p->compileTried = true;
                    std::string err;
                    p->regex = OnigRegex::compile(src, err);
                    if (!p->regex && std::getenv("MORENO_SYNTAX_DEBUG")) std::fprintf(stderr, "regex error (%s): %s\n", err.c_str(), src.c_str());
                }
                if (!p->regex) continue;
                OnigRegex::Match m;
                if (!p->regex->search(line, pos, anchor, m)) continue;
                long start = m.groups[0].first;
                if (best == nullptr || start < bestStart) { best = p; bestMatch = std::move(m); bestStart = start; if (start == static_cast<long>(pos)) break; }
            }
            // an embed context's escape beats everything inside it
            if (frame.temp && frame.temp->name == "__embed") {
                // handled through its first pattern (the escape), which collect() lists first
            }
            if (std::getenv("MORENO_SYNTAX_TRACE")) std::fprintf(stderr, "pos=%zu ctx=%s candidates=%zu best=%s\n", pos, frame.ctx->name.c_str(), candidates.size(), best ? best->source.c_str() : "(none)");
            if (!best) { currentScopes(scopeText); emit(lineStart + pos, lineStart + line.size(), scopeText); pos = line.size(); break; }

            size_t ms = static_cast<size_t>(bestMatch.groups[0].first), me = static_cast<size_t>(bestMatch.groups[0].second);
            if (best->action == Pattern::Fail) {
                // undo back to the branch point and try its next alternative
                long found = -1;
                for (size_t k = snapshots.size(); k-- > 0;) if (snapshots[k].name == best->failName) { found = static_cast<long>(k); break; }
                if (found >= 0) {
                    Snapshot snap = snapshots[static_cast<size_t>(found)];
                    snapshots.resize(static_cast<size_t>(found));
                    stack = snap.stack;
                    scopes = snap.scopes;
                    spans.resize(snap.spanCount);
                    if (!spans.empty()) spans.back().length = snap.lastSpanLength;
                    altPattern = snap.pattern; altLine = snap.lineStart; altPos = snap.pos; altIndex = snap.nextAlternative;
                    jumpPending = true; jumpLine = snap.lineStart; jumpPos = snap.pos;
                    return;
                }
                // no branch to undo: treat as a plain empty match
                size_t step = std::max(pos + 1, me);
                currentScopes(scopeText);
                emit(lineStart + pos, lineStart + std::min(step, line.size()), scopeText);
                pos = std::min(step, line.size());
                anchor = pos;
                continue;
            }
            size_t branchIndex = 0;
            if (best->action == Pattern::Branch) {
                if (altPattern == best && altLine == lineStart && altPos == pos) branchIndex = altIndex;
                altPattern = nullptr;
                if (branchIndex >= best->branchTargets.size()) branchIndex = best->branchTargets.empty() ? 0 : best->branchTargets.size() - 1;
                Snapshot snap;
                snap.name = best->branchPoint; snap.pattern = best; snap.lineStart = lineStart; snap.pos = pos;
                snap.stack = stack; snap.scopes = scopes; snap.spanCount = spans.size(); snap.lastSpanLength = spans.empty() ? 0 : spans.back().length;
                snap.nextAlternative = branchIndex + 1;
                if (branchIndex + 1 < best->branchTargets.size()) snapshots.push_back(std::move(snap));
                if (snapshots.size() > 64) snapshots.erase(snapshots.begin());
            }
            currentScopes(scopeText);
            emit(lineStart + pos, lineStart + ms, scopeText);

            // resolve push/set targets before emitting the match so meta_scope applies to it
            std::vector<Resolved> targets;
            if (best->action == Pattern::Push || best->action == Pattern::Set || best->action == Pattern::Branch || best->kind == Pattern::Embed) {
                const SublimeSyntax::Impl* patternOwner = best->owner ? best->owner : owner;
                std::vector<std::string> wanted = best->targets;
                if (best->action == Pattern::Branch && branchIndex < best->branchTargets.size()) wanted = {best->branchTargets[branchIndex]};
                for (auto& t : wanted) {
                    const SublimeSyntax::Impl* foundOwner = nullptr;
                    const Context* c = find(*patternOwner, t, &foundOwner);
                    if (c) targets.push_back({c, nullptr});
                }
                for (auto& inl : best->inlineTargets) targets.push_back({inl.get(), inl});
                if (best->kind == Pattern::Embed) {
                    auto temp = std::make_shared<Context>();
                    temp->name = "__embed";
                    temp->owner = owner;
                    temp->metaContentScope = best->embedScopeNames;
                    Pattern esc;
                    esc.owner = patternOwner;
                    esc.source = best->escape;
                    esc.captures = best->escapeCaptures;
                    esc.action = Pattern::Pop;
                    temp->patterns.push_back(esc);
                    Pattern inc;
                    inc.owner = patternOwner;
                    inc.kind = Pattern::Include;
                    inc.includeName = best->embedScope;
                    temp->patterns.push_back(inc);
                    temps.push_back(temp);
                    targets.push_back({temp.get(), temp});
                }
            }

            // scopes for the matched text
            std::vector<std::string> matchScopes = scopes;
            long escapeFrame = -1;
            for (auto& e : escapes) if (e.first == best) escapeFrame = static_cast<long>(e.second);
            if (escapeFrame >= 0) {
                size_t drop = 0;
                for (size_t j = static_cast<size_t>(escapeFrame); j < stack.size(); ++j) drop += stack[j].metaCount + stack[j].contentCount;
                matchScopes.resize(matchScopes.size() >= drop ? matchScopes.size() - drop : 0);
            } else if (best->action == Pattern::Pop) {
                size_t drop = stack.back().contentCount;
                matchScopes.resize(matchScopes.size() >= drop ? matchScopes.size() - drop : 0);
            } else {
                for (auto& t : targets) { for (auto& s : t.ctx->metaScope) matchScopes.push_back(s); }
            }
            for (auto& s : best->scope) matchScopes.push_back(s);
            auto join = [](const std::vector<std::string>& v) { std::string o; for (size_t i = 0; i < v.size(); ++i) { if (i) o += ' '; o += v[i]; } return o; };

            if (best->captures.empty()) {
                emit(lineStart + ms, lineStart + me, join(matchScopes));
            } else {
                // split the match around its capture groups
                std::vector<std::pair<long, long>> bounds;
                size_t cursor = ms;
                std::vector<std::tuple<size_t, size_t, int>> caps;
                for (auto& [group, names] : best->captures) {
                    if (group < 0 || static_cast<size_t>(group) >= bestMatch.groups.size()) continue;
                    auto g = bestMatch.groups[static_cast<size_t>(group)];
                    if (g.first < 0 || g.second <= g.first) continue;
                    caps.emplace_back(static_cast<size_t>(g.first), static_cast<size_t>(g.second), group);
                }
                std::sort(caps.begin(), caps.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b) || (std::get<0>(a) == std::get<0>(b) && std::get<1>(a) > std::get<1>(b)); });
                std::string base = join(matchScopes);
                for (auto& [cs, ce, group] : caps) {
                    if (cs < cursor) continue;  // nested capture: the outer group already covered it
                    emit(lineStart + cursor, lineStart + cs, base);
                    std::vector<std::string> inner = matchScopes;
                    for (auto& s : best->captures.at(group)) inner.push_back(s);
                    emit(lineStart + cs, lineStart + ce, join(inner));
                    cursor = ce;
                }
                emit(lineStart + cursor, lineStart + me, base);
            }

            // state change
            if (escapeFrame >= 0) {
                while (stack.size() > static_cast<size_t>(escapeFrame)) popFrame();
            } else if (best->kind == Pattern::Embed) {
                if (best->action == Pattern::Pop) for (int i = 0; i < best->popCount; ++i) popFrame();  // "pop: 1" together with embed
                for (auto& t : targets) pushFrame(t.ctx, nullptr, t.keep);
            } else if (best->action == Pattern::Pop) {
                for (int i = 0; i < best->popCount; ++i) popFrame();
            } else if (best->action == Pattern::Set || (best->kind == Pattern::Embed)) {
                if (best->action == Pattern::Set) popFrame();
                for (auto& t : targets) pushFrame(t.ctx, best->withPrototype.empty() ? nullptr : &best->withPrototype, t.keep);
            } else if (best->action == Pattern::Push || best->action == Pattern::Branch) {
                for (auto& t : targets) pushFrame(t.ctx, best->withPrototype.empty() ? nullptr : &best->withPrototype, t.keep);
            }
            // a branch whose context has been popped again is settled: it cannot be undone any more
            while (!snapshots.empty() && stack.size() <= snapshots.back().stack.size()) snapshots.pop_back();
            bool changed = best->action != Pattern::None || best->kind == Pattern::Embed;
            if (me == pos && !changed) {
                // an empty match that changes nothing: step over one character
                size_t step = pos + 1;
                while (step < line.size() && (static_cast<unsigned char>(line[step]) & 0xC0) == 0x80) ++step;
                currentScopes(scopeText);
                emit(lineStart + pos, lineStart + step, scopeText);
                pos = step;
            } else {
                pos = std::max(me, pos);
            }
            anchor = pos;
        }
        if (pos < line.size()) { currentScopes(scopeText); emit(lineStart + pos, lineStart + line.size(), scopeText); }
    }
};

}  // namespace

std::vector<ScopeSpan> SublimeSyntax::tokenize(const std::string& text, const SyntaxLibrary* library) const {
    static std::mutex tokenizeMutex;  // patterns compile their regexes lazily, so one tokenizer runs at a time
    std::lock_guard<std::mutex> lock(tokenizeMutex);
    Tokenizer t(*impl_, library);
    return t.run(text);
}

// ── Library ──────────────────────────────────────────────────────────────────────────────────────────────

void SyntaxLibrary::add(std::shared_ptr<SublimeSyntax> syntax) { if (syntax) syntaxes_.push_back(std::move(syntax)); }

std::shared_ptr<SublimeSyntax> SyntaxLibrary::byScope(const std::string& scope) const {
    for (auto& s : syntaxes_) if (s->scope() == scope) return s;
    return nullptr;
}

std::shared_ptr<SublimeSyntax> SyntaxLibrary::byExtension(const std::string& ext) const {
    for (auto& s : syntaxes_) for (auto& e : s->extensions()) if (e == ext) return s;
    return nullptr;
}

std::shared_ptr<SublimeSyntax> SyntaxLibrary::byName(const std::string& name) const {
    for (auto& s : syntaxes_) if (s->name() == name) return s;
    return nullptr;
}

size_t SyntaxLibrary::loadDirectory(const std::string& dir) {
    size_t added = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".sublime-syntax") continue;
        std::ifstream in(it->path(), std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string err;
        auto syn = SublimeSyntax::parse(text, err);
        if (syn) { add(syn); ++added; }
    }
    return added;
}

// ── Reading .sublime-package archives ────────────────────────────────────────────────────────────────────

namespace {

uint32_t rd32(const std::string& d, size_t o) { return static_cast<uint8_t>(d[o]) | (static_cast<uint8_t>(d[o + 1]) << 8) | (static_cast<uint8_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d[o + 3])) << 24); }
uint16_t rd16(const std::string& d, size_t o) { return static_cast<uint16_t>(static_cast<uint8_t>(d[o]) | (static_cast<uint8_t>(d[o + 1]) << 8)); }

bool inflateEntry(const std::string& data, size_t offset, uint32_t compSize, uint32_t rawSize, uint16_t method, std::string& out) {
    if (offset + compSize > data.size()) return false;
    if (method == 0) { out.assign(data, offset, compSize); return true; }
    if (method != 8) return false;
    out.resize(rawSize);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data() + offset));
    zs.avail_in = compSize;
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = rawSize;
    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
}

}  // namespace

size_t SyntaxLibrary::loadPackage(const std::string& packagePath) {
    std::ifstream in(packagePath, std::ios::binary);
    if (!in) return 0;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 22) return 0;
    size_t eocd = std::string::npos;
    for (size_t i = data.size() - 22 + 1; i-- > 0 && data.size() - i < 65557;) if (rd32(data, i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return 0;
    uint16_t count = rd16(data, eocd + 10);
    size_t pos = rd32(data, eocd + 16);
    size_t added = 0;
    for (uint16_t n = 0; n < count && pos + 46 <= data.size(); ++n) {
        if (rd32(data, pos) != 0x02014b50) break;
        uint16_t method = rd16(data, pos + 10);
        uint32_t comp = rd32(data, pos + 20), raw = rd32(data, pos + 24);
        uint16_t nameLen = rd16(data, pos + 28), extraLen = rd16(data, pos + 30), commentLen = rd16(data, pos + 32);
        uint32_t localOffset = rd32(data, pos + 42);
        std::string name = data.substr(pos + 46, nameLen);
        pos += 46u + nameLen + extraLen + commentLen;
        if (name.size() < 15 || name.compare(name.size() - 15, 15, ".sublime-syntax") != 0) continue;
        if (localOffset + 30 > data.size() || rd32(data, localOffset) != 0x04034b50) continue;
        size_t dataOffset = localOffset + 30u + rd16(data, localOffset + 26) + rd16(data, localOffset + 28);
        std::string text;
        if (!inflateEntry(data, dataOffset, comp, raw, method, text)) continue;
        std::string err;
        auto syn = SublimeSyntax::parse(text, err);
        if (syn) { syn->impl_->sourcePath = "Packages/" + fs::path(packagePath).stem().string() + "/" + name; add(syn); ++added; }
        else if (std::getenv("MORENO_SYNTAX_DEBUG")) std::fprintf(stderr, "syntax failed: %s: %s", name.c_str(), err.c_str()), std::fputc(10, stderr);
    }
    return added;
}

size_t SyntaxLibrary::loadPaths(const std::vector<std::string>& dirs) {
    size_t added = 0;
    std::error_code ec;
    for (const std::string& dir : dirs) {
        if (!fs::exists(dir, ec)) continue;
        added += loadDirectory(dir);
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".sublime-package") added += loadPackage(it->path().string());
    }
    resolveExtends();
    return added;
}

SyntaxLibrary& SyntaxLibrary::shared() {
    static SyntaxLibrary library;
    return library;
}

// ── Inheritance (extends) ────────────────────────────────────────────────────────────────────────────────

namespace {

std::shared_ptr<Context> cloneContext(const Context& src, const SublimeSyntax::Impl* owner) {
    auto copy = std::make_shared<Context>(src);
    copy->owner = owner;
    for (Pattern& p : copy->patterns) {
        p.regex.reset();
        p.compileTried = false;
        p.owner = owner;
        for (auto& inl : p.inlineTargets) inl = cloneContext(*inl, owner);
        for (auto& wp : p.withPrototype) wp = cloneContext(*wp, owner);
    }
    return copy;
}

void reexpand(Context& ctx, const std::map<std::string, std::string>& vars) {
    for (Pattern& p : ctx.patterns) {
        if (!p.rawSource.empty()) p.source = expandVariables(p.rawSource, vars);
        if (!p.rawEscape.empty()) p.escape = expandVariables(p.rawEscape, vars);
        p.regex.reset();
        p.compileTried = false;
        for (auto& inl : p.inlineTargets) reexpand(*inl, vars);
        for (auto& wp : p.withPrototype) reexpand(*wp, vars);
    }
}

}  // namespace

void SyntaxLibrary::resolveExtends() {
    // a syntax is resolved once every parent it names is
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        for (auto& syn : syntaxes_) {
            SublimeSyntax::Impl& impl = *syn->impl_;
            if (impl.resolved) continue;
            std::vector<std::shared_ptr<SublimeSyntax>> parents;
            bool ready = true;
            for (const std::string& ref : impl.extends) {
                std::shared_ptr<SublimeSyntax> found;
                std::string base = fs::path(ref).filename().string();
                std::string pkgDir = fs::path(impl.sourcePath).parent_path().string();
                for (auto& cand : syntaxes_) {
                    if (cand == syn) continue;
                    const std::string& path = cand->impl_->sourcePath;
                    if (path == ref) { found = cand; break; }
                }
                if (!found)
                    for (auto& cand : syntaxes_) {
                        if (cand == syn) continue;
                        const std::string& path = cand->impl_->sourcePath;
                        if (fs::path(path).filename().string() == base && fs::path(path).parent_path().string() == pkgDir) { found = cand; break; }
                    }
                if (!found)
                    for (auto& cand : syntaxes_) {
                        if (cand == syn) continue;
                        if (fs::path(cand->impl_->sourcePath).filename().string() == base) { found = cand; break; }
                    }
                if (!found) { ready = false; break; }
                if (!found->impl_->resolved) { ready = false; break; }
                parents.push_back(found);
            }
            if (!ready) continue;
            // variables: the first parent's, overlaid by later parents, overlaid by this syntax's own
            std::map<std::string, std::string> vars;
            for (auto& parent : parents) for (auto& kv : parent->impl_->variables) vars[kv.first] = kv.second;
            for (auto& kv : impl.variables) vars[kv.first] = kv.second;
            std::map<std::string, std::shared_ptr<Context>> own = std::move(impl.contexts);
            impl.contexts.clear();
            for (auto& parent : parents) {
                for (auto& kv : parent->impl_->contexts) {
                    auto inherited = cloneContext(*kv.second, &impl);
                    auto existing = impl.contexts.find(kv.first);
                    if (existing == impl.contexts.end()) { impl.contexts[kv.first] = inherited; continue; }
                    // two parents define it: later ones append after earlier ones
                    existing->second->patterns.insert(existing->second->patterns.end(), inherited->patterns.begin(), inherited->patterns.end());
                }
            }
            for (auto& kv : own) {
                auto mine = kv.second;
                mine->owner = &impl;
                auto inherited = impl.contexts.find(kv.first);
                if (inherited == impl.contexts.end() || (!mine->metaPrepend && !mine->metaAppend)) { impl.contexts[kv.first] = mine; continue; }
                std::shared_ptr<Context> merged = inherited->second;
                if (mine->hasMetaScope) { merged->metaScope = mine->metaScope; merged->hasMetaScope = true; }
                if (mine->hasMetaContentScope) { merged->metaContentScope = mine->metaContentScope; merged->hasMetaContentScope = true; }
                if (mine->metaPrepend) merged->patterns.insert(merged->patterns.begin(), mine->patterns.begin(), mine->patterns.end());
                else merged->patterns.insert(merged->patterns.end(), mine->patterns.begin(), mine->patterns.end());
            }
            impl.variables = vars;
            for (auto& kv : impl.contexts) reexpand(*kv.second, vars);
            if (impl.baseScope.empty()) impl.baseScope = syn->scope_;
            impl.resolved = true;
            changed = true;
        }
        if (!changed) break;
    }
    // syntaxes that never resolved (a missing parent) are dropped
    syntaxes_.erase(std::remove_if(syntaxes_.begin(), syntaxes_.end(), [](const std::shared_ptr<SublimeSyntax>& s) { return !s->impl_->resolved || !s->impl_->contexts.count("main"); }), syntaxes_.end());
}
