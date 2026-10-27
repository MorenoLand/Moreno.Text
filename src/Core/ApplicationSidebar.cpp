// Sidebar context menu actions and the small text prompt they use.
#include "Core/Application.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

bool validName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    return name.find_first_of("/\\:*?\"<>|") == std::string::npos;
}

}  // namespace

void Application::openPrompt(const std::string& caption, const std::string& initial, std::function<void(const std::string&)> callback) {
    promptActive_ = true;
    promptCaption_ = caption;
    promptCallback_ = std::move(callback);
    goto_.active = true; goto_.query = initial; goto_.selected = 0;
    goto_.items.clear(); goto_.subtexts.clear(); goto_.data.clear(); goto_.scores.clear();
}

void Application::refreshSidebarNode(SidebarNode& node) {
    std::vector<std::string> expandedChildren;
    for (auto& child : node.children) if (child.folder && child.expanded) expandedChildren.push_back(child.path);
    buildSidebarNode(node);
    for (auto& child : node.children) {
        if (child.folder && std::find(expandedChildren.begin(), expandedChildren.end(), child.path) != expandedChildren.end()) {
            child.expanded = true;
            refreshSidebarNode(child);
        }
    }
}

void Application::refreshSidebarTree() {
    if (sidebarRoot_.empty()) return;
    refreshSidebarNode(sidebarTree_);
}

bool Application::createPath(const std::string& dir, const std::string& name, bool folder, std::string& created) {
    if (!validName(name)) { pluginStatusText_ = "Invalid name"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    fs::path target = fs::path(dir) / name;
    std::error_code ec;
    if (fs::exists(target, ec)) { pluginStatusText_ = "\"" + name + "\" already exists"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    if (folder) fs::create_directories(target, ec);
    else { std::ofstream out(target, std::ios::binary); if (!out) ec = std::make_error_code(std::errc::io_error); }
    if (ec) { pluginStatusText_ = "Couldn't create \"" + name + "\""; pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    created = target.string();
    refreshSidebarTree();
    return true;
}

bool Application::renamePath(const std::string& oldPath, const std::string& newName) {
    if (!validName(newName)) { pluginStatusText_ = "Invalid name"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    fs::path from = oldPath, to = from.parent_path() / newName;
    std::error_code ec;
    if (from == to) return true;
    if (fs::exists(to, ec)) { pluginStatusText_ = "\"" + newName + "\" already exists"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    fs::rename(from, to, ec);
    if (ec) { pluginStatusText_ = "Couldn't rename: " + ec.message(); pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    saveCurrentTab();
    for (auto& tab : tabs_) {
        if (tab.filePath == from.string()) { tab.filePath = to.string(); tab.fileName = to.filename().string(); }
        else if (tab.filePath.size() > from.string().size() && tab.filePath.compare(0, from.string().size(), from.string()) == 0 &&
                 (tab.filePath[from.string().size()] == '\\' || tab.filePath[from.string().size()] == '/')) {
            tab.filePath = to.string() + tab.filePath.substr(from.string().size());
        }
    }
    if (activeTab_ < tabs_.size()) { openFilePath_ = tabs_[activeTab_].filePath; openFile_ = tabs_[activeTab_].fileName; }
    refreshSidebarTree();
    return true;
}

bool Application::deletePath(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
    bool ok = false;
#ifdef _WIN32
    std::string doubleNull = path;
    doubleNull.push_back('\0');
    SHFILEOPSTRUCTA op{};
    op.wFunc = FO_DELETE;
    op.pFrom = doubleNull.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    ok = SHFileOperationA(&op) == 0 && !op.fAnyOperationsAborted;
#else
    ok = fs::remove_all(path, ec) > 0 && !ec;
#endif
    if (!ok) { pluginStatusText_ = "Couldn't delete that item"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return false; }
    // Close tabs that showed the deleted file.
    saveCurrentTab();
    for (size_t i = tabs_.size(); i-- > 0;) {
        const std::string& tabPath = tabs_[i].filePath;
        if (tabPath == path || (tabPath.size() > path.size() && tabPath.compare(0, path.size(), path) == 0 && (tabPath[path.size()] == '\\' || tabPath[path.size()] == '/'))) {
            tabs_[i].dirty = false;
            closeTab(i);
        }
    }
    refreshSidebarTree();
    return true;
}

void Application::sidebarContextAction(int action, const SidebarNode& node) {
    std::string dir = node.folder ? node.path : fs::path(node.path).parent_path().string();
    switch (action) {
        case 0:  // New File
            openPrompt("New file name", "", [this, dir](const std::string& name) {
                std::string created;
                if (createPath(dir, name, false, created)) openFile(created);
            });
            break;
        case 7:  // New Folder
            openPrompt("New folder name", "", [this, dir](const std::string& name) {
                std::string created;
                createPath(dir, name, true, created);
            });
            break;
        case 1: {  // Rename
            std::string path = node.path;
            openPrompt("Rename to", fs::path(node.path).filename().string(), [this, path](const std::string& name) { renamePath(path, name); });
            break;
        }
        case 8: {  // Delete (to the recycle bin on Windows)
            std::string path = node.path;
            openPrompt(std::string("Type yes to move ") + fs::path(node.path).filename().string() + " to the recycle bin", "", [this, path](const std::string& answer) {
                if (answer == "yes" || answer == "y") deletePath(path);
            });
            break;
        }
        case 3:  // Open Terminal Here
#ifdef _WIN32
        {
            std::string args = "/K cd /d \"" + dir + "\"";
            ShellExecuteA(nullptr, "open", "cmd.exe", args.c_str(), dir.c_str(), SW_SHOWNORMAL);
        }
#endif
            break;
        case 9:  // Find in Folder
            findScopeDir_ = dir;
            commandFindInFiles();
            break;
        case 6:  // Folder/File History
        case 12: {  // Blame
            std::string target = node.path;
            std::string command = action == 12 ? "git blame -- \"" + target + "\"" : "git log --follow --stat -n 40 -- \"" + target + "\"";
            nlohmann::json spec = {{"shell_cmd", command}, {"working_dir", dir}};
            startBuild(spec, action == 12 ? "Blame" : "History");
            break;
        }
        default: break;
    }
}
