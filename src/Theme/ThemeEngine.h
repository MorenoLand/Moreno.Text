#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <regex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <nlohmann/json.hpp>

struct ThemeColor { float r, g, b, a; };

class ThemeEngine {
public:
    static ThemeEngine& instance() {
        static ThemeEngine t;
        return t;
    }

    // Reads Moreno's own scheme format and Sublime's .sublime-color-scheme JSON
    // (variables, var() references, color(... alpha(x)), comma separated scopes).
    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB) text.erase(0, 3);
        static const std::regex trailingComma(R"(,(\s*[}\]]))");
        text = std::regex_replace(text, trailingComma, "$1");
        auto data = nlohmann::json::parse(text, nullptr, false, true);
        if (data.is_discarded() || !data.is_object()) return;
        path_ = path;
        resetColors();
        name_ = data.value("name", std::string());
        std::unordered_map<std::string, std::string> vars;
        if (data.contains("variables") && data["variables"].is_object())
            for (auto& [k, v] : data["variables"].items()) if (v.is_string()) vars[k] = v.get<std::string>();
        auto color = [&](const nlohmann::json& v, ThemeColor& out) {
            if (v.is_string()) out = resolveColor(v.get<std::string>(), vars, 0);
        };
        if (data.contains("globals") && data["globals"].is_object()) {
            auto& g = data["globals"];
            if (g.contains("background")) color(g["background"], bgColor_);
            if (g.contains("foreground")) color(g["foreground"], fgColor_);
            if (g.contains("caret")) color(g["caret"], caretColor_);
            if (g.contains("line_highlight")) color(g["line_highlight"], lineHighlight_);
            if (g.contains("selection")) color(g["selection"], selectionColor_);
            if (g.contains("gutter")) color(g["gutter"], gutterColor_); else gutterColor_ = bgColor_;
            if (g.contains("gutter_foreground")) color(g["gutter_foreground"], gutterFg_);
            if (g.contains("minimap_background")) color(g["minimap_background"], minimapBg_); else minimapBg_ = bgColor_;
        }
        if (data.contains("rules") && data["rules"].is_array()) {
            for (auto& rule : data["rules"]) {
                if (!rule.is_object() || !rule.contains("scope") || !rule["scope"].is_string() || !rule.contains("foreground")) continue;
                ThemeColor c = fgColor_;
                color(rule["foreground"], c);
                std::string scopes = rule["scope"].get<std::string>();
                size_t start = 0;
                while (start <= scopes.size()) {
                    size_t comma = scopes.find(',', start);
                    std::string one = scopes.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                    size_t a = one.find_first_not_of(" \t"), b = one.find_last_not_of(" \t");
                    if (a != std::string::npos) scopeColors_[one.substr(a, b - a + 1)] = c;
                    if (comma == std::string::npos) break;
                    start = comma + 1;
                }
            }
        }
        // Moreno highlights with a handful of coarse scopes; borrow the closest rule from richer schemes.
        static const std::pair<const char*, std::vector<const char*>> fallbacks[] = {
            {"keyword", {"keyword.control", "storage"}},
            {"number", {"constant.numeric", "constant"}},
            {"type", {"storage.type", "entity.name.type", "entity.name.class", "support.type", "support.class"}},
            {"function", {"entity.name.function", "support.function", "variable.function"}},
            {"operator", {"keyword.operator"}},
        };
        for (auto& [coarse, options] : fallbacks) {
            if (scopeColors_.count(coarse)) continue;
            for (auto option : options) {
                auto it = scopeColors_.find(option);
                if (it != scopeColors_.end()) { scopeColors_[coarse] = it->second; break; }
            }
        }
    }
    const std::string& name() const { return name_; }
    const std::string& path() const { return path_; }
    ThemeColor colorForScope(const std::string& scope) const {
        // try exact match first, then prefix match
        auto it = scopeColors_.find(scope);
        if (it != scopeColors_.end()) return it->second;
        for (auto& [k, v] : scopeColors_) {
            if (scope.find(k) == 0) return v;
        }
        return fgColor_;
    }
    ThemeColor bgColor() const { return bgColor_; }
    ThemeColor fgColor() const { return fgColor_; }
    ThemeColor caretColor() const { return caretColor_; }
    ThemeColor lineHighlightColor() const { return lineHighlight_; }
    ThemeColor selectionColor() const { return selectionColor_; }
    ThemeColor gutterColor() const { return gutterColor_; }
    ThemeColor gutterFg() const { return gutterFg_; }
    ThemeColor minimapBg() const { return minimapBg_; }

    void writeDefault(const std::string& path) { writeScheme(path, "Moreno Dark", "#1e1e22", "#d4d4d8", "#528bff", "#2a2a30", "#264f78", "#1a1a1e", "#6a6a78", "#151518",
        {{"keyword", "#c678dd"}, {"string", "#98c379"}, {"comment", "#5c6370"}, {"number", "#d19a66"}, {"operator", "#56b6c2"}, {"function", "#61afef"},
         {"type", "#e5c07b"}, {"variable", "#e06c75"}, {"punctuation", "#abb2bf"}, {"constant", "#d19a66"}, {"tag", "#e06c75"}, {"attribute", "#d19a66"}}); }
    void writeLight(const std::string& path) { writeScheme(path, "Moreno Light", "#fafafa", "#383a42", "#526fff", "#eeeeee", "#d7e3f4", "#f2f2f2", "#9d9d9f", "#ececec",
        {{"keyword", "#a626a4"}, {"string", "#50a14f"}, {"comment", "#a0a1a7"}, {"number", "#986801"}, {"operator", "#0184bc"}, {"function", "#4078f2"},
         {"type", "#c18401"}, {"variable", "#e45649"}, {"punctuation", "#383a42"}, {"constant", "#986801"}, {"tag", "#e45649"}, {"attribute", "#986801"}}); }
    void writeMonokai(const std::string& path) { writeScheme(path, "Moreno Monokai", "#272822", "#f8f8f2", "#f8f8f0", "#3e3d32", "#49483e", "#272822", "#90908a", "#1e1f1c",
        {{"keyword", "#f92672"}, {"string", "#e6db74"}, {"comment", "#75715e"}, {"number", "#ae81ff"}, {"operator", "#f92672"}, {"function", "#a6e22e"},
         {"type", "#66d9ef"}, {"variable", "#f8f8f2"}, {"punctuation", "#f8f8f2"}, {"constant", "#ae81ff"}, {"tag", "#f92672"}, {"attribute", "#a6e22e"}}); }

private:
    ThemeEngine() = default;

    void writeScheme(const std::string& path, const char* name, const char* bg, const char* fg, const char* caret, const char* lineHl,
                     const char* selection, const char* gutter, const char* gutterFg, const char* minimapBg,
                     const std::vector<std::pair<const char*, const char*>>& rules) {
        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json ruleList = nlohmann::json::array();
        for (auto& [scope, color] : rules) ruleList.push_back({{"scope", scope}, {"foreground", color}});
        nlohmann::json j = {
            {"name", name},
            {"globals", {{"background", bg}, {"foreground", fg}, {"caret", caret}, {"line_highlight", lineHl}, {"selection", selection},
                         {"gutter", gutter}, {"gutter_foreground", gutterFg}, {"minimap_background", minimapBg}}},
            {"rules", ruleList}
        };
        f << j.dump(2);
    }

    ThemeColor parseColor(const std::string& hex) const {
        ThemeColor c = {0.83f, 0.83f, 0.85f, 1.f};
        if (hex.size() >= 7 && hex[0] == '#') {
            unsigned int r = 0, g = 0, b = 0, a = 255;
            sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
            if (hex.size() >= 9) sscanf(hex.c_str() + 7, "%02x", &a);
            c.r = r / 255.f; c.g = g / 255.f; c.b = b / 255.f; c.a = a / 255.f;
        } else if (hex.size() == 4 && hex[0] == '#') {
            unsigned int r = 0, g = 0, b = 0;
            sscanf(hex.c_str() + 1, "%1x%1x%1x", &r, &g, &b);
            c.r = (r * 17) / 255.f; c.g = (g * 17) / 255.f; c.b = (b * 17) / 255.f;
        }
        return c;
    }

    // Resolves "#rrggbb[aa]", "var(name)" and color(<base> alpha(x)); other color() modifiers are ignored.
    ThemeColor resolveColor(const std::string& value, const std::unordered_map<std::string, std::string>& vars, int depth) const {
        if (depth > 8) return fgColor_;
        size_t a = value.find_first_not_of(" \t"), b = value.find_last_not_of(" \t");
        if (a == std::string::npos) return fgColor_;
        std::string v = value.substr(a, b - a + 1);
        if (v.rfind("var(", 0) == 0 && v.back() == ')') {
            std::string key = v.substr(4, v.size() - 5);
            auto it = vars.find(key);
            return it == vars.end() ? fgColor_ : resolveColor(it->second, vars, depth + 1);
        }
        if (v.rfind("color(", 0) == 0 && v.back() == ')') {
            std::string inner = v.substr(6, v.size() - 7);
            size_t baseEnd = 0;
            int paren = 0;
            for (; baseEnd < inner.size(); ++baseEnd) {
                if (inner[baseEnd] == '(') ++paren;
                else if (inner[baseEnd] == ')') --paren;
                else if (inner[baseEnd] == ' ' && paren == 0 && baseEnd > 0) break;
            }
            ThemeColor c = resolveColor(inner.substr(0, baseEnd), vars, depth + 1);
            size_t alphaAt = inner.find("alpha(", baseEnd);
            size_t shortAt = inner.find(" a(", baseEnd);
            size_t open = alphaAt != std::string::npos ? alphaAt + 6 : (shortAt != std::string::npos ? shortAt + 3 : std::string::npos);
            if (open != std::string::npos) {
                float alpha = static_cast<float>(std::atof(inner.c_str() + open));
                c.a = alpha > 1.f ? alpha / 100.f : alpha;
            }
            return c;
        }
        return parseColor(v);
    }

    void resetColors() {
        bgColor_ = {0.118f, 0.118f, 0.133f, 1.f};
        fgColor_ = {0.83f, 0.83f, 0.85f, 1.f};
        caretColor_ = {0.322f, 0.545f, 1.f, 1.f};
        lineHighlight_ = {0.165f, 0.165f, 0.188f, 1.f};
        selectionColor_ = {0.149f, 0.310f, 0.470f, 1.f};
        gutterColor_ = {0.102f, 0.102f, 0.118f, 1.f};
        gutterFg_ = {0.416f, 0.416f, 0.470f, 1.f};
        minimapBg_ = {0.082f, 0.082f, 0.094f, 1.f};
        scopeColors_.clear();
    }

    ThemeColor bgColor_ = {0.118f, 0.118f, 0.133f, 1.f};
    ThemeColor fgColor_ = {0.83f, 0.83f, 0.85f, 1.f};
    ThemeColor caretColor_ = {0.322f, 0.545f, 1.f, 1.f};
    ThemeColor lineHighlight_ = {0.165f, 0.165f, 0.188f, 1.f};
    ThemeColor selectionColor_ = {0.149f, 0.310f, 0.470f, 1.f};
    ThemeColor gutterColor_ = {0.102f, 0.102f, 0.118f, 1.f};
    ThemeColor gutterFg_ = {0.416f, 0.416f, 0.470f, 1.f};
    ThemeColor minimapBg_ = {0.082f, 0.082f, 0.094f, 1.f};
    std::unordered_map<std::string, ThemeColor> scopeColors_;
    std::string path_, name_;
};
