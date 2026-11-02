// Scope names at a position (derived from the highlighter's token classes) and Sublime selector matching.
#include "Core/Application.h"
#include "Syntax/SyntaxHighlighter.h"
#include <algorithm>
#include <sstream>

#ifdef None
#undef None
#endif

namespace {

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

// "comment" matches "comment" and "comment.line" but not "commentary".
bool prefixMatches(const std::string& selectorTerm, const std::string& scope) {
    if (scope.compare(0, selectorTerm.size(), selectorTerm) != 0) return false;
    return scope.size() == selectorTerm.size() || scope[selectorTerm.size()] == '.';
}

}  // namespace

std::string Application::scopeAt(size_t pos) const {
    if (syntax_->usesSublimeSyntax()) {  // the real scopes from the .sublime-syntax
        std::string real = syntax_->scopeNameAt(pos > 0 ? pos - 1 : 0);
        if (!real.empty()) return real;
    }
    std::string lang = syntax_->languageName();
    std::string lower = lang;
    for (auto& c : lower) c = c == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string base;
    if (lower.empty() || lower == "plain-text" || lower == "text") base = "text.plain";
    else if (lower == "markdown") base = "text.html.markdown";
    else if (lower == "html") base = "text.html.basic";
    else base = "source." + lower;
    if (binaryView_ || largeFileGuarded_ || pos > textBuffer.size()) return base;
    size_t ls = lineStart(pos), le = lineEnd(pos);
    auto tokens = syntax_->highlightLine(std::string_view(textBuffer.data() + ls, le - ls), ls);
    for (auto& t : tokens) {
        // A cursor at the end of a token counts as inside it, except for strings and comments, which end before it.
        bool inside = pos > t.start && pos <= t.start + t.length;
        if (t.scope == 2 || t.scope == 3) inside = pos > t.start && pos < t.start + t.length;
        if (!inside) continue;
        switch (t.scope) {
            case 1: return base + " keyword.control";
            case 2: return base + " string.quoted";
            case 3: return base + " comment.line";
            case 4: return base + " constant.numeric";
            case 5: return base + " storage.type";
            case 6: return base + " support.function";
            case 7: return base + " keyword.operator";
            default: break;
        }
    }
    return base;
}

// Union of comma-separated alternatives; each is a descendant path with optional "- term" exclusions.
bool Application::scopeSelectorMatches(const std::string& selector, const std::string& scope) {
    std::vector<std::string> path = words(scope);
    std::stringstream alts(selector);
    std::string alt;
    while (std::getline(alts, alt, ',')) {
        std::vector<std::string> tokens = words(alt);
        std::vector<std::string> positive, negative;
        bool negate = false;
        for (auto& tok : tokens) {
            if (tok == "-") { negate = true; continue; }
            if (tok[0] == '-' && tok.size() > 1) { negative.push_back(tok.substr(1)); continue; }
            (negate ? negative : positive).push_back(tok);
            negate = false;
        }
        size_t at = 0;
        bool ok = true;
        for (auto& term : positive) {
            while (at < path.size() && !prefixMatches(term, path[at])) ++at;
            if (at >= path.size()) { ok = false; break; }
            ++at;
        }
        if (!ok) continue;
        for (auto& term : negative)
            for (auto& elem : path) if (prefixMatches(term, elem)) { ok = false; break; }
        if (ok) return true;
    }
    return false;
}
