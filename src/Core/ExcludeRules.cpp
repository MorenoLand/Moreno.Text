#include "Core/ExcludeRules.h"
#include "Settings/SettingsManager.h"
#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>

namespace {

char fold(char c) {
#ifdef _WIN32
    return c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#else
    return c == '\\' ? '/' : c;
#endif
}

std::vector<std::string> listFrom(const nlohmann::json& object, const char* key) {
    std::vector<std::string> out;
    if (object.is_object() && object.contains(key) && object[key].is_array())
        for (auto& item : object[key]) if (item.is_string()) out.push_back(item.get<std::string>());
    return out;
}

}  // namespace

ExcludeRules ExcludeRules::defaults() {
    ExcludeRules r;
    r.folders = {".svn", ".git", ".hg", "CVS", ".Trash", ".Trash-*"};
    r.files = {"*.pyc", "*.pyo", "*.exe", "*.dll", "*.obj", "*.o", "*.a", "*.lib", "*.so", "*.dylib", "*.ncb", "*.sdf", "*.suo",
               "*.pdb", "*.idb", ".DS_Store", "*.class", "*.psd", "*.db", "*.sublime-workspace"};
    r.binary = {"*.jpg", "*.jpeg", "*.png", "*.gif", "*.ttf", "*.tga", "*.dds", "*.ico", "*.eot", "*.pdf", "*.swf", "*.jar", "*.zip"};
    return r;
}

ExcludeRules ExcludeRules::fromSettings() {
    ExcludeRules r = defaults();
    auto& settings = SettingsManager::instance();
    auto load = [&](const char* key, std::vector<std::string>& into) {
        auto set = settings.get<std::vector<std::string>>(key, std::vector<std::string>());
        // an explicit list in the settings file replaces the default, even an empty one
        nlohmann::json probe = settings.get<nlohmann::json>(key, nlohmann::json());
        if (probe.is_array()) into = set;
    };
    load("folder_exclude_patterns", r.folders);
    load("file_exclude_patterns", r.files);
    load("binary_file_patterns", r.binary);
    return r;
}

void ExcludeRules::merge(const nlohmann::json& object) {
    auto add = [](std::vector<std::string>& into, const std::vector<std::string>& more) {
        for (auto& m : more) if (std::find(into.begin(), into.end(), m) == into.end()) into.push_back(m);
    };
    add(folders, listFrom(object, "folder_exclude_patterns"));
    add(files, listFrom(object, "file_exclude_patterns"));
    add(binary, listFrom(object, "binary_file_patterns"));
}

void ExcludeRules::add(const ExcludeRules& more) {
    auto addAll = [](std::vector<std::string>& into, const std::vector<std::string>& extra) {
        for (auto& m : extra) if (std::find(into.begin(), into.end(), m) == into.end()) into.push_back(m);
    };
    addAll(folders, more.folders); addAll(files, more.files); addAll(binary, more.binary);
}

bool ExcludeRules::glob(const char* p, const char* t) {
    const char* starP = nullptr;
    const char* starT = nullptr;
    while (*t) {
        if (*p == '*') { starP = ++p; starT = t; continue; }
        bool ok = false;
        const char* next = p;
        if (*p == '?') { ok = true; next = p + 1; }
        else if (*p == '[') {
            const char* q = p + 1;
            bool negate = *q == '!' || *q == '^';
            if (negate) ++q;
            bool in = false;
            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') { if (fold(*t) >= fold(*q) && fold(*t) <= fold(q[2])) in = true; q += 3; }
                else { if (fold(*t) == fold(*q)) in = true; ++q; }
            }
            if (*q == ']') { ok = in != negate; next = q + 1; }
        }
        else if (*p && fold(*p) == fold(*t)) { ok = true; next = p + 1; }
        if (ok) { p = next; ++t; continue; }
        if (starP) { p = starP; t = ++starT; continue; }
        return false;
    }
    while (*p == '*') ++p;
    return *p == 0;
}

bool ExcludeRules::matches(const std::string& pattern, const std::string& name, const std::string& fullPath) {
    if (pattern.find('/') != std::string::npos || pattern.find('\\') != std::string::npos) return glob(pattern.c_str(), fullPath.c_str());
    return glob(pattern.c_str(), name.c_str());
}

bool ExcludeRules::excludesFolder(const std::string& name, const std::string& fullPath) const {
    for (auto& p : folders) if (matches(p, name, fullPath)) return true;
    return false;
}

bool ExcludeRules::excludesFile(const std::string& name, const std::string& fullPath) const {
    for (auto& p : files) if (matches(p, name, fullPath)) return true;
    return false;
}

bool ExcludeRules::isBinary(const std::string& name, const std::string& fullPath) const {
    for (auto& p : binary) if (matches(p, name, fullPath)) return true;
    return false;
}
