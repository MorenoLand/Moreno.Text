// file_exclude_patterns, folder_exclude_patterns and binary_file_patterns: which files the sidebar, Goto Anything,
// Find in Files and the symbol index leave out. Defaults match Sublime's; settings replace them, and a project
// folder can add its own patterns.
#pragma once
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct ExcludeRules {
    std::vector<std::string> folders, files, binary;

    static ExcludeRules defaults();
    // The defaults, with any of the three lists the user's settings file sets replacing them.
    static ExcludeRules fromSettings();
    // Adds the patterns a project (or one of its folder entries) lists on top of these.
    void merge(const nlohmann::json& object);
    void add(const ExcludeRules& more);

    bool excludesFolder(const std::string& name, const std::string& fullPath) const;
    bool excludesFile(const std::string& name, const std::string& fullPath) const;   // sidebar, Goto, Find in Files
    bool isBinary(const std::string& name, const std::string& fullPath) const;       // Goto and Find in Files only

    // Glob with * and ? (and [abc] sets). A pattern with a slash is matched against the whole path, any other against the name.
    static bool matches(const std::string& pattern, const std::string& name, const std::string& fullPath);
    static bool glob(const char* pattern, const char* text);
};
