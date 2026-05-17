#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <chrono>

class Titlebar;
class Gutter;
class Minimap;
class StatusBar;
class SyntaxHighlighter;

struct AppPaths {
    std::string exeDir, dataDir, packagesDir, installedPackagesDir;
    std::string cacheDir, localDir, libDir, backupDir;
    bool portable = false;
};

struct SelRange {
    size_t anchor = 0, cursor = 0;
    SelRange() = default;
    SelRange(size_t pos) : anchor(pos), cursor(pos) {}
    SelRange(size_t a, size_t c) : anchor(a), cursor(c) {}
    bool hasSelection() const { return anchor != cursor; }
    size_t min() const { return anchor < cursor ? anchor : cursor; }
    size_t max() const { return anchor < cursor ? cursor : anchor; }
};

struct UndoStep {
    std::string text;
    size_t cursorPos;
    std::chrono::steady_clock::time_point time;
};

struct FindState {
    bool active = false;
    bool replaceActive = false;
    std::string query;
    std::string replace;
    bool regex = false;
    bool caseSensitive = false;
    bool wholeWord = false;
    std::vector<size_t> matches;
    size_t currentMatch = 0;
};

struct GotoState {
    bool active = false;
    std::string query;
    std::vector<std::string> items;
    std::vector<float> scores;
    int selected = 0;
};

struct TabBuffer {
    std::string text;
    std::string filePath;
    std::string fileName = "untitled";
    std::vector<SelRange> selections;
    float scrollY = 0.f;
    std::vector<UndoStep> undoStack, redoStack;
    std::vector<bool> foldedLines;
    bool dirty = false;
    float desiredCursorX = -1.f;
};

enum class StatusPopup { None, Indent, Syntax };

class Application {
public:
    static Application& instance();
    int run(int argc, char** argv);
    const AppPaths& paths() const { return paths_; }
    void quit() { running_ = false; }
    SyntaxHighlighter& syntax() { return *syntax_; }
    void newBuffer();
    void saveFile();
    void saveFileAs();
    void openFile(const std::string& path);
    void switchToTab(size_t index);
    void closeTab(size_t index);
private:
    Application() = default;
    bool init(int argc, char** argv);
    void shutdown();
    void initPaths();
    void handleEvents();
    void update();
    void render();
    void updateTitle();
    size_t lineStart(size_t pos) const;
    size_t lineEnd(size_t pos) const;
    size_t lineStartForLine(size_t line) const;
    size_t lineOfPos(size_t pos) const;
    size_t totalLines() const;
    size_t colOfPos(size_t pos) const;
    void insertAtCursor(const std::string& text);
    void deleteSelection();
    void insertText(const std::string& text);
    void findAllMatches();
    void ensureCursorVisible();
    void detectSyntax();
    void pushUndo();
    void doUndo();
    void doRedo();
    void doReplace();
    void doReplaceAll();
    // tabs
    std::vector<TabBuffer> tabs_;
    size_t activeTab_ = 0;
    void saveCurrentTab();
    void loadTab(size_t index);
    void drawTabBar(class FontAtlas& font, float windowW, float titlebarH);
    bool handleTabBarEvent(const SDL_Event& e, float windowW, float titlebarH);
    float tabBarH_ = 35.f;
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    AppPaths paths_;
    bool running_ = true, dirty_ = false;
    std::string textBuffer;
    std::string openFilePath_;
    std::string openFile_ = "untitled";
    std::vector<SelRange> selections_;
    float desiredCursorX_ = -1.f;
    float scrollY_ = 0.f;
    bool selecting_ = false;
    bool minimapDragging_ = false;
    bool scrollbarHovered_ = false;
    bool scrollbarDragging_ = false;
    float scrollbarDragOffset_ = 0.f;
    int mouseX_ = 0, mouseY_ = 0;
    float tabScrollX_ = 0.f;
    bool tabDropdownOpen_ = false;
    int tabDropdownHover_ = -1;
    std::vector<UndoStep> undoStack_, redoStack_;
    std::chrono::milliseconds undoWindow_{500};
    FindState find_;
    GotoState goto_;
    std::vector<bool> foldedLines_;
    void toggleFold(size_t line);
    bool isFolded(size_t line) const;
    size_t findFoldEnd(size_t startLine) const;
    std::vector<int> lineIndents_;
    void computeLineIndents();
    bool useTabs_ = false;
    int tabSize_ = 2;
    std::string menuStyle_ = "icon";
    void convertIndentation(bool toSpaces);
    void guessIndent();
    StatusPopup statusPopup_ = StatusPopup::None;
    int popupSelected_ = 0;
    float popupX_ = 0, popupY_ = 0;
    int syntaxLangIndex_ = -1;
    static const char* syntaxLanguages[];
    static constexpr int syntaxLangCount = 26;
    Titlebar* titlebar_ = nullptr;
    Gutter* gutter_ = nullptr;
    Minimap* minimap_ = nullptr;
    StatusBar* statusbar_ = nullptr;
    SyntaxHighlighter* syntax_ = nullptr;
    friend SDL_HitTestResult hitTestCallback(SDL_Window*, const SDL_Point*, void*);
};
