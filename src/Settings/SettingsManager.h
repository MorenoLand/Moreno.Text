#pragma once
#include <string>
#include <unordered_map>
#include <fstream>
#include <regex>
#include <nlohmann/json.hpp>

class SettingsManager {
public:
    static SettingsManager& instance() {
        static SettingsManager s;
        return s;
    }
    void load(const std::string& path) {
        path_ = path;
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB) text.erase(0, 3);
            static const std::regex trailingComma(R"(,(\s*[}\]]))");
            text = std::regex_replace(text, trailingComma, "$1");
            auto parsed = nlohmann::json::parse(text, nullptr, false, true);
            if (parsed.is_object()) data_ = std::move(parsed);
        }
    }
    void save() {
        std::ofstream f(path_);
        if (f.is_open()) f << data_.dump(2);
    }
    void ensureDefaults() {
        setDefault("font_face", "Consolas");
        setDefault("font_size", 12);
        setDefault("sidebar_font_size", 12);
        setDefault("font_bold", false);
        setDefault("tab_size", 2);
        setDefault("translate_tabs_to_spaces", true);
        setDefault("word_wrap", false);
        setDefault("highlight_line", true);
        setDefault("line_numbers", true);
        setDefault("scroll_past_end", true);
        setDefault("draw_white_space", std::string("selection"));
        setDefault("auto_match_enabled", true);
        setDefault("hot_exit", true);
        setDefault("remember_open_files", true);
        setDefault("trim_trailing_white_space_on_save", false);
        setDefault("ensure_newline_at_eof_on_save", false);
        setDefault("match_brackets", true);
        setDefault("minimap_visible", true);
        setDefault("sidebar_visible", false);
    }
    template<typename T>
    T get(const std::string& key, const T& def) const {
        if (!data_.contains(key)) return def;
        try { return data_[key].get<T>(); }
        catch (...) { return def; }
    }
    template<typename T>
    void set(const std::string& key, const T& val) {
        data_[key] = val;
    }
    void reload() {
        std::ifstream f(path_);
        if (f.is_open()) {
            try { auto newData = nlohmann::json::parse(f); data_ = newData; }
            catch (...) {}
        }
    }
    const std::string& path() const { return path_; }
private:
    SettingsManager() = default;
    std::string path_;
    nlohmann::json data_;
    template<typename T>
    void setDefault(const std::string& key, const T& val) {
        if (!data_.contains(key)) data_[key] = val;
    }
};
