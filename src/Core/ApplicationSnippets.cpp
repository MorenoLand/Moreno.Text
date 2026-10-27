// Snippets: .sublime-snippet discovery, tab-trigger expansion and tab stops.
#include "Core/Application.h"
#include "Syntax/SyntaxHighlighter.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string xmlText(const std::string& body, const char* tag) {
    std::string open = std::string("<") + tag + ">", close = std::string("</") + tag + ">";
    size_t a = body.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    size_t b = body.find(close, a);
    if (b == std::string::npos) return {};
    std::string text = body.substr(a, b - a);
    size_t cdata = text.find("<![CDATA[");
    if (cdata != std::string::npos) {
        size_t start = cdata + 9, end = text.find("]]>", start);
        if (end != std::string::npos) return text.substr(start, end - start);
    }
    static const std::pair<const char*, const char*> entities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (auto& [from, to] : entities) {
        size_t at = 0;
        while ((at = text.find(from, at)) != std::string::npos) { text.replace(at, std::strlen(from), to); at += std::strlen(to); }
    }
    size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

}  // namespace

void Application::loadSnippets() {
    uint32_t now = SDL_GetTicks();
    if (!snippets_.empty() && now - snippetsLoadedAt_ < 20000) return;
    snippets_.clear();
    std::vector<std::string> roots = {paths_.packagesDir, (fs::path(paths_.cacheDir) / "Installed Packages").string(), paths_.installedPackagesDir};
    for (auto& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".sublime-snippet") continue;
            std::ifstream in(it->path(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            SnippetDef def;
            def.content = xmlText(body, "content");
            if (def.content.empty()) continue;
            def.trigger = xmlText(body, "tabTrigger");
            def.description = xmlText(body, "description");
            def.scope = xmlText(body, "scope");
            if (def.description.empty()) def.description = def.trigger.empty() ? it->path().stem().string() : def.trigger;
            snippets_.push_back(std::move(def));
        }
    }
    snippetsLoadedAt_ = now ? now : 1;
}

bool Application::snippetScopeMatches(const std::string& scope) const {
    if (scope.empty()) return true;
    std::string lang = lowered(syntax_->languageName());
    if (lang.empty()) return true;
    std::string sc = lowered(scope);
    std::string dashed = lang;
    std::replace(dashed.begin(), dashed.end(), ' ', '-');
    return sc.find(lang) != std::string::npos || sc.find(dashed) != std::string::npos || sc.rfind("text", 0) == 0;
}

void Application::updateSnippetResults(const std::string& pattern) {
    loadSnippets();
    std::string needle = lowered(pattern);
    for (size_t i = 0; i < snippets_.size() && goto_.items.size() < 200; ++i) {
        const SnippetDef& s = snippets_[i];
        if (!needle.empty() && lowered(s.description).find(needle) == std::string::npos && lowered(s.trigger).find(needle) == std::string::npos) continue;
        goto_.items.push_back(s.description);
        goto_.subtexts.push_back(s.trigger.empty() ? s.scope : s.trigger);
        goto_.data.push_back(std::to_string(i));
    }
}

void Application::openSnippetPicker() { openGotoWithPrefix("$"); }

void Application::insertSnippetByIndex(size_t index) {
    loadSnippets();
    if (index < snippets_.size()) insertSnippet(snippets_[index].content);
}

bool Application::expandSnippetTrigger() {
    if (binaryView_ || largeFileGuarded_ || selections_.empty() || selections_[0].hasSelection()) return false;
    loadSnippets();
    if (snippets_.empty()) return false;
    size_t cursor = selections_[0].cursor, start = cursor;
    while (start > 0 && !std::isspace(static_cast<unsigned char>(textBuffer[start - 1])) && textBuffer[start - 1] != '(' && textBuffer[start - 1] != ')' && textBuffer[start - 1] != '{' && textBuffer[start - 1] != ';') --start;
    std::string word = textBuffer.substr(start, cursor - start);
    if (word.empty()) return false;
    for (const SnippetDef& s : snippets_) {
        if (s.trigger != word || !snippetScopeMatches(s.scope)) continue;
        pushUndo();
        textBuffer.erase(start, cursor - start);
        selections_.clear(); selections_.emplace_back(start);
        invalidateLineIndex();
        insertSnippet(s.content);
        return true;
    }
    return false;
}

void Application::insertSnippet(const std::string& content) {
    if (binaryView_ || largeFileGuarded_ || selections_.empty()) return;
    pushUndo();
    SelRange sel = selections_[0];
    std::string selected = sel.hasSelection() ? textBuffer.substr(sel.min(), sel.max() - sel.min()) : std::string();
    size_t at = sel.min();
    size_t ls = lineStart(at), indentEnd = ls;
    while (indentEnd < textBuffer.size() && (textBuffer[indentEnd] == ' ' || textBuffer[indentEnd] == '\t')) ++indentEnd;
    std::string indent = textBuffer.substr(ls, std::min(indentEnd, at) - ls);
    std::string lineText = textBuffer.substr(ls, lineEnd(at) - ls);
    std::string wordText;
    {
        size_t s = at, e = at;
        while (s > 0 && (std::isalnum(static_cast<unsigned char>(textBuffer[s - 1])) || textBuffer[s - 1] == '_')) --s;
        while (e < textBuffer.size() && (std::isalnum(static_cast<unsigned char>(textBuffer[e])) || textBuffer[e] == '_')) ++e;
        wordText = textBuffer.substr(s, e - s);
    }
    std::map<std::string, std::string> vars = {
        {"TM_SELECTED_TEXT", selected}, {"SELECTION", selected}, {"TM_CURRENT_LINE", lineText}, {"TM_CURRENT_WORD", wordText},
        {"TM_FILENAME", fs::path(openFilePath_).filename().string()}, {"TM_FILEPATH", openFilePath_},
        {"TM_LINE_INDEX", std::to_string(at - ls)}, {"TM_LINE_NUMBER", std::to_string(lineOfPos(at) + 1)},
        {"TM_SOFT_TABS", useTabs_ ? "NO" : "YES"}, {"TM_TAB_SIZE", std::to_string(tabSize_)},
        {"TM_FULLNAME", std::getenv("USERNAME") ? std::getenv("USERNAME") : (std::getenv("USER") ? std::getenv("USER") : "")}};

    std::string out;
    struct Stop { size_t start = 0, len = 0; bool placeholder = false; };
    std::map<int, std::vector<Stop>> stops;
    auto emit = [&](char c) { out += c; if (c == '\n') out += indent; };
    auto emitText = [&](const std::string& text) { for (char c : text) emit(c); };
    for (size_t i = 0; i < content.size();) {
        char c = content[i];
        if (c == '\\' && i + 1 < content.size() && (content[i + 1] == '$' || content[i + 1] == '\\' || content[i + 1] == '}')) { emit(content[i + 1]); i += 2; continue; }
        if (c != '$') { emit(c); ++i; continue; }
        size_t j = i + 1;
        bool braced = j < content.size() && content[j] == '{';
        if (braced) ++j;
        size_t nameStart = j;
        while (j < content.size() && (std::isalnum(static_cast<unsigned char>(content[j])) || content[j] == '_')) ++j;
        std::string name = content.substr(nameStart, j - nameStart);
        if (name.empty()) { emit(c); ++i; continue; }
        std::string defaultText;
        bool hasDefault = false;
        if (braced) {
            if (j < content.size() && content[j] == ':') {
                hasDefault = true; ++j;
                int depth = 1;
                size_t d = j;
                for (; d < content.size() && depth > 0; ++d) {
                    if (content[d] == '\\' && d + 1 < content.size()) { ++d; continue; }
                    if (content[d] == '{') ++depth;
                    else if (content[d] == '}') --depth;
                }
                defaultText = content.substr(j, d - j - 1);
                j = d;
            } else {
                while (j < content.size() && content[j] != '}') ++j;
                if (j < content.size()) ++j;
            }
        }
        if (std::isdigit(static_cast<unsigned char>(name[0]))) {
            int index = std::atoi(name.c_str());
            Stop stop;
            stop.start = out.size();
            stop.placeholder = hasDefault;
            if (hasDefault) {
                std::string plain;
                static const std::regex nested(R"(\$\{\d+:([^}]*)\}|\$\d+)");
                plain = std::regex_replace(defaultText, nested, "$1");
                size_t before = out.size();
                emitText(plain);
                stop.len = out.size() - before;
            } else if (!stops[index].empty()) {
                const Stop& first = stops[index].front();
                emitText(out.substr(first.start, first.len));
            }
            stops[index].push_back(stop);
        } else {
            auto it = vars.find(name);
            if (it != vars.end() && !(it->second.empty() && hasDefault)) emitText(it->second);
            else if (hasDefault) emitText(defaultText);
        }
        i = j;
    }
    size_t base = at;
    if (sel.hasSelection()) { textBuffer.erase(sel.min(), sel.max() - sel.min()); }
    textBuffer.insert(base, out);
    invalidateLineIndex(); dirty_ = true;
    snippetStops_.clear();
    std::vector<int> order;
    for (auto& [index, list] : stops) if (index != 0) order.push_back(index);
    for (int index : order) {
        std::vector<std::pair<size_t, size_t>> ranges;
        for (auto& s : stops[index]) ranges.emplace_back(base + s.start, s.len);
        snippetStops_.push_back(std::move(ranges));
    }
    if (stops.count(0)) snippetFinal_ = base + stops[0].front().start; else snippetFinal_ = base + out.size();
    snippetCurrent_ = 0;
    snippetBufSize_ = textBuffer.size();
    snippetActive_ = !snippetStops_.empty();
    selectSnippetStop();
    if (!snippetActive_) { selections_.clear(); selections_.emplace_back(snippetFinal_); }
}

void Application::selectSnippetStop() {
    if (!snippetActive_) return;
    if (snippetCurrent_ >= snippetStops_.size()) {
        selections_.clear(); selections_.emplace_back(std::min(snippetFinal_, textBuffer.size()));
        snippetActive_ = false;
        return;
    }
    auto& ranges = snippetStops_[snippetCurrent_];
    selections_.clear();
    for (auto& r : ranges) selections_.emplace_back(std::min(r.first, textBuffer.size()), std::min(r.first + r.second, textBuffer.size()));
    snippetBufSize_ = textBuffer.size();
    ensureCursorVisible();
}

bool Application::advanceSnippet(bool forward) {
    if (!snippetActive_ || selections_.empty() || snippetCurrent_ >= snippetStops_.size()) return false;
    auto& current = snippetStops_[snippetCurrent_];
    // Typing only edits the first range of the current stop, so the buffer size change is that range's growth.
    long delta = static_cast<long>(textBuffer.size()) - static_cast<long>(snippetBufSize_);
    size_t start = current.front().first;
    long grown = static_cast<long>(current.front().second) + delta;
    if (grown < 0) { snippetActive_ = false; return false; }
    size_t newLen = static_cast<size_t>(grown);
    size_t cursor = selections_[0].cursor;
    if (cursor < start || cursor > start + newLen || start + newLen > textBuffer.size()) { snippetActive_ = false; return false; }
    auto shiftAfter = [&](size_t pos, long amount, const std::pair<size_t, size_t>* skip) {
        for (auto& stop : snippetStops_)
            for (auto& r : stop) if (&r != skip && r.first > pos) r.first = static_cast<size_t>(static_cast<long>(r.first) + amount);
        if (snippetFinal_ > pos) snippetFinal_ = static_cast<size_t>(static_cast<long>(snippetFinal_) + amount);
    };
    shiftAfter(start, delta, &current.front());
    current.front().second = newLen;
    if (current.size() > 1) {
        std::string typed = textBuffer.substr(start, newLen);
        std::vector<size_t> order;
        for (size_t k = 1; k < current.size(); ++k) order.push_back(k);
        std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return current[x].first > current[y].first; });
        for (size_t k : order) {
            size_t pos = current[k].first, len = current[k].second;
            if (pos + len > textBuffer.size()) continue;
            textBuffer.replace(pos, len, typed);
            long diff = static_cast<long>(typed.size()) - static_cast<long>(len);
            shiftAfter(pos, diff, &current[k]);
            current[k].second = typed.size();
        }
    }
    invalidateLineIndex(); dirty_ = true;
    if (forward) ++snippetCurrent_;
    else if (snippetCurrent_ > 0) --snippetCurrent_;
    selectSnippetStop();
    return true;
}

void Application::wrapSelectionWithTag() {
    if (binaryView_ || largeFileGuarded_ || selections_.empty()) return;
    pushUndo();
    SelRange sel = selections_[0];
    size_t a = sel.min(), b = sel.max();
    std::string inner = textBuffer.substr(a, b - a);
    std::string wrapped = "<p>" + inner + "</p>";
    textBuffer.replace(a, b - a, wrapped);
    selections_.clear();
    selections_.emplace_back(a + 1, a + 2);
    selections_.emplace_back(a + 3 + inner.size() + 2, a + 3 + inner.size() + 3);
    invalidateLineIndex(); dirty_ = true;
}
