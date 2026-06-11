#pragma once
#include <string>
#include <vector>

struct PluginHostPaths {
    std::string exeDir, dataDir, packagesDir, installedPackagesDir;
    std::string cacheDir, libDir;
};

class PluginHost {
public:
    struct Script {
        std::string package;
        std::string path;
        std::string archive;
        std::string relativePath;
    };
    struct Command {
        std::string caption;
        std::string name;
        std::string script;
    };
    static std::vector<Script> discoverScripts(const std::string& packagesDir);
    static std::vector<Command> discoverCommands(const std::vector<Script>& scripts);
    static std::vector<Command> discoverCommandPaletteEntries(const std::string& packagesDir);
    static std::string commandNameFromClass(const std::string& className);
    static std::string commandCaptionFromName(const std::string& name);
    static std::string hostExecutablePath(const PluginHostPaths& paths);
    static bool writeBootstrapFiles(const PluginHostPaths& paths);
    static void start(const PluginHostPaths& paths);
    static void runCommand(const PluginHostPaths& paths, const std::string& commandName);
private:
    static std::string legacyApiName();
    static bool writeCommandRequest(const PluginHostPaths& paths, const std::string& commandName);
    static void launchHost(const PluginHostPaths& paths, const std::string& commandName, bool persistent);
};
