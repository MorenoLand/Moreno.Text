// Projects with more than one folder: the first lives in sidebarRoot_/sidebarTree_, the rest in extraFolders_.
#include "Core/Application.h"
#include "Platform/Platform.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <filesystem>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

std::vector<SidebarNode*> Application::sidebarRoots() {
    std::vector<SidebarNode*> roots = {&sidebarTree_};
    for (auto& extra : extraFolders_) roots.push_back(&extra);
    return roots;
}

std::vector<std::string> Application::projectRoots() {
    std::vector<std::string> roots;
    if (!sidebarRoot_.empty()) roots.push_back(sidebarRoot_);
    for (auto& extra : extraFolders_) roots.push_back(extra.path);
    return roots;
}

void Application::addFolderToProject(const std::string& path) {
    std::error_code ec;
    if (!fs::is_directory(path, ec)) return;
    if (sidebarRoot_.empty()) { loadFolder(path); return; }
    auto same = [&](const std::string& other) { return fs::equivalent(path, other, ec); };
    if (same(sidebarRoot_)) return;
    for (auto& extra : extraFolders_) if (same(extra.path)) return;
    SidebarNode node;
    node.name = fs::path(path).filename().string();
    if (node.name.empty()) node.name = path;
    node.path = path;
    node.folder = true;
    node.expanded = true;
    buildSidebarNode(node);
    extraFolders_.push_back(std::move(node));
    sidebarVisible_ = true;
}

void Application::removeFolderFromProject(const std::string& path) {
    std::error_code ec;
    for (size_t i = 0; i < extraFolders_.size(); ++i) {
        if (extraFolders_[i].path == path || fs::equivalent(extraFolders_[i].path, path, ec)) {
            extraFolders_.erase(extraFolders_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
    if (sidebarRoot_.empty() || !(path == sidebarRoot_ || fs::equivalent(path, sidebarRoot_, ec))) return;
    if (!extraFolders_.empty()) {
        std::string next = extraFolders_.front().path;
        std::vector<SidebarNode> rest(extraFolders_.begin() + 1, extraFolders_.end());
        loadFolder(next);
        extraFolders_ = std::move(rest);
        return;
    }
    sidebarRoot_.clear(); sidebarTree_ = {}; sidebarVisible_ = false;
    saveSession();
}

void Application::addFolderDialog() {
    closeAllPopups();
    std::string path;
    inOsDialog_ = true;
    if (Platform::pickFolder(path)) addFolderToProject(path);
    inOsDialog_ = false;
}
