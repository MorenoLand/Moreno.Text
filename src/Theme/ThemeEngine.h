#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <nlohmann/json.hpp>

struct ThemeColor { float r, g, b, a; };

class ThemeEngine {
public:
    static ThemeEngine& instance() {
        static ThemeEngine t;
        return t;
    }
    void load(const std::string& path) {
        path_ = path;
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            auto data = nlohmann::json::parse(f);
            if (data.contains("globals")) {
                auto& g = data["globals"];
                if (g.contains("background")) bgColor_ = parseColor(g["background"]);
                if (g.contains("foreground")) fgColor_ = parseColor(g["foreground"]);
                if (g.contains("caret")) caretColor_ = parseColor(g["caret"]);
                if (g.contains("line_highlight")) lineHighlight_ = parseColor(g["line_highlight"]);
                if (g.contains("selection")) selectionColor_ = parseColor(g["selection"]);
                if (g.contains("gutter")) gutterColor_ = parseColor(g["gutter"]);
                if (g.contains("gutter_foreground")) gutterFg_ = parseColor(g["gutter_foreground"]);
                if (g.contains("minimap_background")) minimapBg_ = parseColor(g["minimap_background"]);
            }
            if (data.contains("rules")) {
                for (auto& rule : data["rules"]) {
                    if (!rule.contains("scope") || !rule.contains("foreground")) continue;
                    std::string scope = rule["scope"];
                    ThemeColor c = parseColor(rule["foreground"]);
                    scopeColors_[scope] = c;
                }
            }
        } catch (...) {}
    }
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
    void writeDefault(const std::string& path) {
        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j = {
            {"name", "Moreno Dark"},
            {"globals", {
                {"background", "#1e1e22"}, {"foreground", "#d4d4d8"},
                {"caret", "#528bff"}, {"line_highlight", "#2a2a30"},
                {"selection", "#264f78"}, {"gutter", "#1a1a1e"},
                {"gutter_foreground", "#6a6a78"}, {"minimap_background", "#151518"}
            }},
            {"rules", {
                {{"scope", "keyword"}, {"foreground", "#c678dd"}},
                {{"scope", "string"}, {"foreground", "#98c379"}},
                {{"scope", "comment"}, {"foreground", "#5c6370"}},
                {{"scope", "number"}, {"foreground", "#d19a66"}},
                {{"scope", "operator"}, {"foreground", "#56b6c2"}},
                {{"scope", "function"}, {"foreground", "#61afef"}},
                {{"scope", "type"}, {"foreground", "#e5c07b"}},
                {{"scope", "variable"}, {"foreground", "#e06c75"}},
                {{"scope", "punctuation"}, {"foreground", "#abb2bf"}},
                {{"scope", "constant"}, {"foreground", "#d19a66"}},
                {{"scope", "tag"}, {"foreground", "#e06c75"}},
                {{"scope", "attribute"}, {"foreground", "#d19a66"}}
            }}
        };
        f << j.dump(2);
    }
private:
    ThemeEngine() = default;
    ThemeColor parseColor(const std::string& hex) const {
        ThemeColor c = {0.83f, 0.83f, 0.85f, 1.f};
        if (hex.size() >= 7 && hex[0] == '#') {
            unsigned int r, g, b;
            sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
            c.r = r / 255.f; c.g = g / 255.f; c.b = b / 255.f;
        }
        return c;
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
    std::string path_;
};
