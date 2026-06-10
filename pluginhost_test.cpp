#include "Plugin/PluginHost.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

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
