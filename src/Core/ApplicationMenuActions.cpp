// Actions the nested menus call directly: syntax by name, indentation settings.
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include "Syntax/SyntaxHighlighter.h"
#include <algorithm>

#ifdef None
#undef None
#endif

void Application::convertIndentationMenu(bool toSpaces) { convertIndentation(toSpaces); }

void Application::setSyntaxByName(const std::string& name) {
    if (name == "Automatic") {
        if (activeTab_ < tabs_.size()) tabs_[activeTab_].manualSyntaxName.clear();
        detectSyntax();
    } else {
        syntax_->setLanguageByName(name);
        if (activeTab_ < tabs_.size()) tabs_[activeTab_].manualSyntaxName = name;
    }
    syntaxDirty_ = true; indentsDirty_ = true;
    syntax_->parse(textBuffer);
}

void Application::setTabWidth(int width) {
    tabSize_ = std::clamp(width, 1, 16);
    syntax_->setTabSize(tabSize_);
    SettingsManager::instance().set("tab_size", tabSize_);
    SettingsManager::instance().save();
}

void Application::setIndentUsingSpaces(bool spaces) {
    useTabs_ = !spaces;
    syntax_->setUseTabs(useTabs_);
    SettingsManager::instance().set("translate_tabs_to_spaces", spaces);
    SettingsManager::instance().save();
}

void Application::detectIndentation() { guessIndent(); }

std::vector<std::string> Application::encodingNames() {
    std::vector<std::string> names;
    extern std::vector<std::string> allEncodingNames();
    return allEncodingNames();
}

void Application::clearRecentFiles() {
    recentFiles_.clear();
    saveRecentFiles();
}

// Stops looking at Find in Files results and closes the find panel.
void Application::cancelFindInFiles() {
    find_.active = false;
    findPanelIsInFiles_ = false;
    pluginStatusText_ = "Find in Files cancelled";
    pluginStatusUntil_ = SDL_GetTicks() + 2000;
}

// Mac OS 9 line endings: the buffer keeps LF, and the file is written with CR.
void Application::useMacLineEndings() {
    convertLineEndings(false);
    if (!openFilePath_.empty()) macLineEndingFiles_.insert(openFilePath_);
    dirty_ = true;
}
