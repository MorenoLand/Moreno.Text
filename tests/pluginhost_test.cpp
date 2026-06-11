#include "Plugin/PluginHost.h"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void put16(std::vector<unsigned char>& out, uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
}

static void put32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xff));
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xff));
}

static void writeStoredZip(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<unsigned char> out;
    struct Central { std::string name; uint32_t offset; uint32_t size; };
    std::vector<Central> central;
    for (const auto& [name, content] : files) {
        uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, 0);
        put16(out, 0); put16(out, 0); put32(out, 0);
        put32(out, static_cast<uint32_t>(content.size()));
        put32(out, static_cast<uint32_t>(content.size()));
        put16(out, static_cast<uint16_t>(name.size())); put16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), content.begin(), content.end());
        central.push_back({name, offset, static_cast<uint32_t>(content.size())});
    }
    uint32_t centralOffset = static_cast<uint32_t>(out.size());
    for (const auto& item : central) {
        put32(out, 0x02014b50); put16(out, 20); put16(out, 20); put16(out, 0); put16(out, 0);
        put16(out, 0); put16(out, 0); put32(out, 0); put32(out, item.size); put32(out, item.size);
        put16(out, static_cast<uint16_t>(item.name.size())); put16(out, 0); put16(out, 0);
        put16(out, 0); put16(out, 0); put32(out, 0); put32(out, item.offset);
        out.insert(out.end(), item.name.begin(), item.name.end());
    }
    uint32_t centralSize = static_cast<uint32_t>(out.size()) - centralOffset;
    put32(out, 0x06054b50); put16(out, 0); put16(out, 0);
    put16(out, static_cast<uint16_t>(central.size()));
    put16(out, static_cast<uint16_t>(central.size()));
    put32(out, centralSize); put32(out, centralOffset); put16(out, 0);
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

int main() {
    fs::path base = fs::temp_directory_path() / "moreno_pluginhost_test";
    fs::remove_all(base);
    fs::create_directories(base / "Packages" / "Example");
    fs::create_directories(base / "Packages" / "User");
    { std::ofstream(base / "Packages" / "Example" / "main.py") << "def plugin_loaded(): pass\n"; }
    { std::ofstream(base / "Packages" / "Example" / "ignored.txt") << ""; }
    { std::ofstream(base / "Packages" / "User" / "skip.py") << ""; }
    auto scripts = PluginHost::discoverScripts((base / "Packages").string());
    if (scripts.size() != 1) return 1;
    if (scripts[0].package != "Example") return 2;
    if (PluginHost::commandNameFromClass("RcRefreshServersCommand") != "rc_refresh_servers") return 3;
    auto commands = PluginHost::discoverCommands(scripts);
    if (!commands.empty()) return 4;
    std::string api = std::string("sub") + "lime_plugin";
    { std::ofstream(base / "Packages" / "Example" / "commands.py") << "import " << api << "\nclass RcRefreshServersCommand(" << api << ".WindowCommand):\n    def run(self): pass\n"; }
    scripts = PluginHost::discoverScripts((base / "Packages").string());
    commands = PluginHost::discoverCommands(scripts);
    if (commands.size() != 1 || commands[0].name != "rc_refresh_servers") return 5;
    std::string commandFileName = std::string("Default.") + std::string("sub") + "lime-commands";
    { std::ofstream(base / "Packages" / "Example" / commandFileName) << "[{\"caption\":\"RC: Connect to GServer\",\"command\":\"rc_connect_server\"},{\"caption\":\"RC: Refresh Servers\",\"command\":\"rc_refresh_servers\"}]"; }
    commands = PluginHost::discoverCommands(scripts);
    if (commands.size() != 2 || commands[0].caption != "RC: Connect to GServer" || commands[0].name != "rc_connect_server") return 6;
    if (commands[1].caption != "RC: Refresh Servers" || commands[1].name != "rc_refresh_servers") return 7;
    fs::remove_all(base / "Packages" / "Example");
    writeStoredZip(base / "Installed Packages" / "ZipPkg.sublime-package", {
        {"plugin.py", "import sublime_plugin\nclass ZipInstallCommand(sublime_plugin.WindowCommand):\n    def run(self): pass\n"},
        {"Default.sublime-commands", "[{\"caption\":\"Package Control: Install Package\",\"command\":\"install_package\"}]"}
    });
    scripts = PluginHost::discoverScripts((base / "Packages").string());
    bool foundZipScript = false;
    for (const auto& script : scripts) if (script.package == "ZipPkg" && fs::path(script.path).filename() == "plugin.py") foundZipScript = true;
    if (!foundZipScript) return 17;
    commands = PluginHost::discoverCommands(scripts);
    bool foundZipCommand = false;
    for (const auto& command : commands) if (command.caption == "Package Control: Install Package" && command.name == "install_package") foundZipCommand = true;
    if (!foundZipCommand) return 18;
    PluginHostPaths paths;
    paths.exeDir = (base / "Bin").string();
    paths.dataDir = (base / "Data").string();
    paths.packagesDir = (base / "Packages").string();
    paths.installedPackagesDir = (base / "Data" / "Installed Packages").string();
    paths.cacheDir = (base / "Data" / "Cache").string();
    paths.libDir = (base / "Data" / "Lib").string();
    if (PluginHost::hostExecutablePath(paths) != (base / "Bin" / "moreno_plugin_host.exe").string()) return 8;
    if (!PluginHost::writeBootstrapFiles(paths)) return 9;
    if (!fs::exists(fs::path(paths.libDir) / "python38" / "moreno_plugin_bootstrap.py")) return 10;
    {
        std::ifstream f(fs::path(paths.libDir) / "python38" / "moreno_plugin_bootstrap.py", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.find("[plugin-host] persistent ready") == std::string::npos) return 11;
        if (text.find("plugin_command") == std::string::npos) return 12;
    }
    {
        std::ifstream f(fs::path(paths.libDir) / "python38" / "sublime.py", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.find("def is_dirty(self):") == std::string::npos) return 13;
        if (text.find("def _dispatch_window_command") == std::string::npos) return 14;
        if (text.find("view_window_command") == std::string::npos) return 15;
        if (text.find("def _dispatch_post_save") == std::string::npos) return 16;
    }
    fs::remove_all(base);
    return 0;
}
