#pragma once
// Colors of the window chrome (title bar, tab bar, side bar, status bar). The default matches the built-in look;
// a .sublime-theme file can override them, or the chrome can follow the active color scheme.
#include "Theme/ThemeEngine.h"
#include <fstream>
#include <regex>
#include <string>

class UiTheme {
public:
    static UiTheme& instance() { static UiTheme t; return t; }

    ThemeColor titleBar = {0.16f, 0.16f, 0.19f, 1.f};
    ThemeColor tabBar = {0.25f, 0.25f, 0.28f, 1.f};
    ThemeColor sideBar = {0.105f, 0.115f, 0.135f, 1.f};
    ThemeColor sideBarEdge = {0.06f, 0.07f, 0.085f, 1.f};
    ThemeColor statusBar = {0.15f, 0.15f, 0.18f, 1.f};
    std::string name = "Default";

    void reset() { *this = UiTheme(); }

    // Dark chrome colors in the source (panels, hover rows, borders) become light ones on a light chrome: the
    // lightness is mirrored and the hue kept, so a blue hover row stays a blue hover row.
    ThemeColor m(float r, float g, float b) const {
        if (!isLight()) return {r, g, b, 1.f};
        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float l = (mx + mn) / 2.f, d = mx - mn;
        float newL = 1.f - l;
        if (d < 0.0001f) return {newL, newL, newL, 1.f};
        float sat = d / (1.f - (l * 2.f - 1.f < 0.f ? -(l * 2.f - 1.f) : (l * 2.f - 1.f)));
        float h;
        if (mx == r) h = (g - b) / d + (g < b ? 6.f : 0.f);
        else if (mx == g) h = (b - r) / d + 2.f;
        else h = (r - g) / d + 4.f;
        float q = newL < 0.5f ? newL * (1.f + sat) : newL + sat - newL * sat, p = 2.f * newL - q;
        auto hue = [&](float t) {
            if (t < 0.f) t += 1.f;
            if (t > 1.f) t -= 1.f;
            if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
            if (t < 0.5f) return q;
            if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
            return p;
        };
        h /= 6.f;
        return {hue(h + 1.f / 3.f), hue(h), hue(h - 1.f / 3.f), 1.f};
    }

    // True while chrome is being drawn; text colors that are plain grays are then inverted on a light chrome.
    static bool& chromeText() { static bool flag = false; return flag; }
    struct ChromeScope {
        bool previous;
        ChromeScope() : previous(chromeText()) { chromeText() = UiTheme::instance().isLight(); }
        ~ChromeScope() { chromeText() = previous; }
    };
    static void textColor(float& r, float& g, float& b) {
        if (!chromeText()) return;
        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn > 0.16f) return;  // accents (links, branch names) keep their color
        r = 1.f - r; g = 1.f - g; b = 1.f - b;
    }
    // Chrome text is dark on a light chrome: callers pass a light-on-dark gray and get the matching one back.
    bool isLight() const { return (tabBar.r + tabBar.g + tabBar.b) / 3.f > 0.6f; }
    float text(float darkThemeGray) const { return isLight() ? 1.f - darkThemeGray : darkThemeGray; }

    // Chrome colors derived from the color scheme's background, a little lighter or darker per element.
    void followColorScheme(const ThemeColor& background, const ThemeColor& gutter) {
        auto shade = [](ThemeColor c, float d) { return ThemeColor{clamp01(c.r + d), clamp01(c.g + d), clamp01(c.b + d), 1.f}; };
        bool dark = (background.r + background.g + background.b) < 1.5f;
        float step = dark ? 0.05f : -0.06f;
        titleBar = shade(background, step * 1.2f);
        tabBar = shade(background, step * 2.2f);
        sideBar = shade(gutter, 0.f);
        sideBarEdge = shade(gutter, -step * 1.2f);
        statusBar = shade(background, step * 1.6f);
        name = "Follow Color Scheme";
    }

    // Reads a Sublime .sublime-theme: a JSON list of rules. Flat themes tint a layer; the "bg" or layer0.tint of the
    // title_bar, tabset_control, sidebar_tree and status_bar classes is used.
    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.erase(0, 3);
        static const std::regex trailing(R"(,(\s*[}\]]))");
        text = std::regex_replace(text, trailing, "$1");
        auto data = nlohmann::json::parse(text, nullptr, false, true);
        if (!data.is_array()) return false;
        UiTheme fresh;
        for (auto& rule : data) {
            if (!rule.is_object() || !rule.contains("class") || !rule["class"].is_string()) continue;
            std::string cls = rule["class"].get<std::string>();
            if (rule.contains("parents") || rule.contains("attributes")) continue;  // only the plain element rules
            ThemeColor c;
            if (!tint(rule, c)) continue;
            if (cls == "title_bar") fresh.titleBar = c;
            else if (cls == "tabset_control") fresh.tabBar = c;
            else if (cls == "sidebar_tree" || cls == "sidebar_container") { fresh.sideBar = c; fresh.sideBarEdge = ThemeColor{clamp01(c.r - 0.04f), clamp01(c.g - 0.04f), clamp01(c.b - 0.04f), 1.f}; }
            else if (cls == "status_bar") fresh.statusBar = c;
        }
        fresh.name = path;
        *this = fresh;
        return true;
    }

private:
    static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
    static bool tint(const nlohmann::json& rule, ThemeColor& out) {
        for (const char* key : {"layer0.tint", "bg", "background_color"}) {
            if (!rule.contains(key)) continue;
            const auto& v = rule[key];
            if (v.is_array() && v.size() >= 3 && v[0].is_number()) { out = {v[0].get<float>() / 255.f, v[1].get<float>() / 255.f, v[2].get<float>() / 255.f, 1.f}; return true; }
            if (v.is_string()) { out = ThemeEngine::instance().parseColorPublic(v.get<std::string>()); return true; }
        }
        return false;
    }
};

// Expands to r, g, b of a dark chrome color after the theme remap: ar(x0, y0, x1, y1, UIC(0.17f, 0.17f, 0.20f), 0.98f)
#define UIC(cr_, cg_, cb_) UiTheme::instance().m(cr_, cg_, cb_).r, UiTheme::instance().m(cr_, cg_, cb_).g, UiTheme::instance().m(cr_, cg_, cb_).b
