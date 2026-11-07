// Color scheme discovery and switching.
#include "Core/Application.h"
#include "Theme/UiTheme.h"
#include "Settings/SettingsManager.h"
#include "Theme/ThemeEngine.h"
#include <SDL2/SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

struct SchemeEntry {
    std::string name, path;
};

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<SchemeEntry> findSchemes(const AppPaths& paths) {
    std::vector<SchemeEntry> out;
    std::vector<std::string> roots = {paths.packagesDir, (fs::path(paths.cacheDir) / "Installed Packages").string(), paths.installedPackagesDir};
    for (auto& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            if (ext != ".sublime-color-scheme" && ext != ".moreno-color-scheme" && ext != ".tmTheme") continue;
            std::string name = it->path().stem().string();
            std::ifstream in(it->path(), std::ios::binary);
            std::string head(512, '\0');
            in.read(head.data(), 512);
            head.resize(static_cast<size_t>(in.gcount()));
            size_t at = head.find("\"name\"");
            if (at != std::string::npos) {
                size_t q1 = head.find('"', head.find(':', at) + 1), q2 = q1 == std::string::npos ? q1 : head.find('"', q1 + 1);
                if (q2 != std::string::npos) name = head.substr(q1 + 1, q2 - q1 - 1);
            }
            out.push_back({name, it->path().string()});
        }
    }
    std::sort(out.begin(), out.end(), [](const SchemeEntry& a, const SchemeEntry& b) { return lowered(a.name) < lowered(b.name); });
    return out;
}

}  // namespace

void Application::updateColorSchemeResults(const std::string& pattern) {
    std::string needle = lowered(pattern);
    for (auto& entry : findSchemes(paths_)) {
        if (!needle.empty() && lowered(entry.name).find(needle) == std::string::npos) continue;
        bool current = entry.path == ThemeEngine::instance().path();
        goto_.items.push_back(entry.name + (current ? "  (current)" : ""));
        goto_.subtexts.push_back(fs::path(entry.path).filename().string());
        goto_.data.push_back(entry.path);
    }
}

void Application::applyColorScheme(const std::string& path) {
    if (!fs::exists(path)) return;
    ThemeEngine::instance().load(path);
    auto& settings = SettingsManager::instance();
    settings.set("color_scheme", path);
    settings.save();
    applyUiThemeSetting();  // a chrome that follows the scheme changes with it
    syntaxDirty_ = true;
    pluginStatusText_ = "Color scheme: " + (ThemeEngine::instance().name().empty() ? fs::path(path).stem().string() : ThemeEngine::instance().name());
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

void Application::openColorSchemePicker() {
    openGotoWithPrefix("=");
}

void Application::openUiThemePicker() {
    openGotoWithPrefix("&");
}

void Application::updateUiThemeResults(const std::string& pattern) {
    std::string needle = lowered(pattern);
    std::string active = SettingsManager::instance().get<std::string>("theme", "Default.sublime-theme");
    auto add = [&](const std::string& label, const std::string& sub, const std::string& value) {
        if (!needle.empty() && lowered(label).find(needle) == std::string::npos) return;
        goto_.items.push_back(label + (value == active ? "  (current)" : ""));
        goto_.subtexts.push_back(sub);
        goto_.data.push_back(value);
    };
    add("Default", "Built-in look", "Default.sublime-theme");
    add("Follow Color Scheme", "Chrome takes its colors from the color scheme", "follow_color_scheme");
    std::error_code ec;
    for (fs::recursive_directory_iterator it(paths_.dataDir + "/Packages", fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".sublime-theme") continue;
        std::string file = it->path().filename().string();
        if (file == "Default.sublime-theme") continue;
        add(it->path().stem().string(), it->path().parent_path().filename().string(), file);
    }
}

void Application::customizeColorScheme() {
    const std::string& path = ThemeEngine::instance().path();
    if (!path.empty()) openFile(path);
}

void Application::loadSavedColorScheme() {
    auto& theme = ThemeEngine::instance();
    std::string defaultDir = paths_.dataDir + "/Packages/Default";
    if (!fs::exists(defaultDir + "/Moreno Light.moreno-color-scheme")) theme.writeLight(defaultDir + "/Moreno Light.moreno-color-scheme");
    if (!fs::exists(defaultDir + "/Moreno Monokai.moreno-color-scheme")) theme.writeMonokai(defaultDir + "/Moreno Monokai.moreno-color-scheme");
    std::string saved = SettingsManager::instance().get<std::string>("color_scheme", "");
    if (!saved.empty() && fs::exists(saved)) theme.load(saved);
    applyUiThemeSetting();
}

// The "theme" setting: "Default.sublime-theme" (the built-in look), "follow_color_scheme", or a .sublime-theme file
// (a path, or a name found under the Packages folder).
void Application::applyUiThemeSetting() {
    std::string value = SettingsManager::instance().get<std::string>("theme", "Default.sublime-theme");
    UiTheme& ui = UiTheme::instance();
    if (value == "follow_color_scheme") {
        ui.followColorScheme(ThemeEngine::instance().bgColor(), ThemeEngine::instance().gutterColor());
        return;
    }
    ui.reset();
    if (value.empty() || value == "Default.sublime-theme") return;
    std::error_code ec;
    std::string path = value;
    if (!fs::exists(path, ec)) {
        path.clear();
        for (fs::recursive_directory_iterator it(paths_.dataDir + "/Packages", fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && it->path().filename() == value) { path = it->path().string(); break; }
        }
    }
    if (!path.empty()) ui.load(path);
}

void Application::setUiTheme(const std::string& value) {
    auto& settings = SettingsManager::instance();
    settings.set("theme", value);
    settings.save();
    applyUiThemeSetting();
    pluginStatusText_ = "UI theme: " + UiTheme::instance().name;
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

void Application::chooseUiThemeFile() {
#ifdef _WIN32
    closeAllPopups();
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Sublime Theme\0*.sublime-theme\0All Files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
    inOsDialog_ = true;
    bool picked = GetOpenFileNameA(&ofn) != 0;
    inOsDialog_ = false;
    if (picked) setUiTheme(buf);
#endif
}

// Opens Packages/User/Default.sublime-theme for editing, creating a small example the first time.
void Application::customizeUiTheme() {
    fs::path file = fs::path(paths_.dataDir) / "Packages" / "User" / "Default.sublime-theme";
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (!fs::exists(file, ec)) {
        std::ofstream out(file);
        out << "[\n\t// Colors for the window chrome. Values are [r, g, b] from 0 to 255.\n\t{ \"class\": \"title_bar\", \"layer0.tint\": [40, 40, 48] },\n\t{ \"class\": \"tabset_control\", \"layer0.tint\": [58, 58, 66] },\n\t{ \"class\": \"sidebar_tree\", \"layer0.tint\": [26, 29, 34] },\n\t{ \"class\": \"status_bar\", \"layer0.tint\": [38, 38, 46] }\n]\n";
    }
    openFile(file.string());
}
