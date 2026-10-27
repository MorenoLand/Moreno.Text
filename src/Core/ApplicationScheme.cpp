// Color scheme discovery and switching.
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include "Theme/ThemeEngine.h"
#include <SDL2/SDL.h>
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
    syntaxDirty_ = true;
    pluginStatusText_ = "Color scheme: " + (ThemeEngine::instance().name().empty() ? fs::path(path).stem().string() : ThemeEngine::instance().name());
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

void Application::openColorSchemePicker() {
    openGotoWithPrefix("=");
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
}
