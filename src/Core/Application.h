#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <set>

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
    std::vector<std::string> subtexts;
    std::vector<float> scores;
    int selected = 0;
};

struct CommandPaletteState {
    bool active = false;
    std::string query;
    std::vector<int> results;
    int selected = 0;
};

struct TabBuffer {
    std::string text;
    std::string filePath;
    std::string fileName = "untitled";
    std::vector<SelRange> selections;
    float scrollY = 0.f;
    std::vector<UndoStep> undoStack, redoStack;
    std::set<std::pair<int,int>> foldedRegions;
    bool dirty = false;
    float desiredCursorX = -1.f;
};

enum class StatusPopup { None, Indent, Syntax };

struct SidebarNode {
    std::string name, path, extension;
    bool folder = false;
    bool expanded = false;
    int depth = 0;
    std::vector<SidebarNode> children;
};

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
    void closeCurrentTab() { closeTab(activeTab_); }
    void reopenClosedTab();
    void openFileDialog();
    void openFolderDialog();
    void toggleSidebar() { sidebarVisible_ = !sidebarVisible_; }
    void saveAll();
    void revertFile();
    void commandUndo() { doUndo(); }
    void commandRedo() { doRedo(); }
    void commandCut() { cutSelectionOrLine(); }
    void commandCopy() { copySelectionOrLine(); }
    void commandPaste() { pasteClipboard(false); }
    void commandPasteAndIndent() { pasteClipboard(true); }
    void commandSelectAll() { if (!selections_.empty()) { selections_[0].anchor = 0; selections_[0].cursor = textBuffer.size(); } }
    void commandFind() { find_.active = true; find_.replaceActive = false; find_.query.clear(); find_.matches.clear(); }
    void commandReplace() { find_.active = true; find_.replaceActive = true; find_.query.clear(); find_.matches.clear(); }
    void commandGotoAnything() { goto_.active = true; goto_.query.clear(); goto_.selected = 0; goto_.items.clear(); }
    void commandPalette() { commandPalette_.active = true; commandPalette_.query.clear(); commandPalette_.selected = 0; updateCommandPalette(); }
    void toggleFullscreen();
    void toggleMinimap() { minimapVisible_ = !minimapVisible_; }
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
    void updateGitBranch();
    void pushUndo();
    void doUndo();
    void doRedo();
    void doReplace();
    void doReplaceAll();
    std::string selectedTextOrLine() const;
    void copySelectionOrLine();
    void cutSelectionOrLine();
    void pasteClipboard(bool andIndent);
    void loadFolder(const std::string& path);
    void buildSidebarNode(SidebarNode& node);
    void drawSidebar(class FontAtlas& font, float windowH, float titlebarH, float statusbarH);
    bool handleSidebarEvent(const SDL_Event& e, float windowH, float titlebarH, float statusbarH);
    // tabs
    std::vector<TabBuffer> tabs_;
    size_t activeTab_ = 0;
    void saveCurrentTab();
    void loadTab(size_t index);
    void closeTabNow(size_t index);
    void drawTabBar(class FontAtlas& font, float windowW, float titlebarH);
    bool handleTabBarEvent(const SDL_Event& e, float windowW, float titlebarH);
    float tabBarH_ = 35.f;
    float tabChevronX_ = 0.f;
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    AppPaths paths_;
    bool running_ = true, dirty_ = false;
    bool syntaxDirty_ = true;
    std::string textBuffer;
    std::string openFilePath_;
    std::string openFile_ = "untitled";
    std::vector<SelRange> selections_;
    float desiredCursorX_ = -1.f;
    float scrollY_ = 0.f;
    bool selecting_ = false;
    bool minimapDragging_ = false;
    bool minimapVisible_ = true;
    bool fullscreen_ = false;
    bool scrollbarHovered_ = false;
    bool scrollbarDragging_ = false;
    float scrollbarDragOffset_ = 0.f;
    int mouseX_ = 0, mouseY_ = 0;
    float tabScrollX_ = 0.f;
    bool tabDropdownOpen_ = false;
    int tabDropdownHover_ = -1;
    bool tabContextOpen_ = false;
    int tabContextHover_ = -1;
    size_t tabContextIndex_ = 0;
    float tabContextX_ = 0.f, tabContextY_ = 0.f;
    bool sidebarVisible_ = false;
    bool sidebarResizing_ = false;
    float sidebarWidth_ = 200.f;
    float sidebarScrollY_ = 0.f;
    float sidebarContentH_ = 0.f;
    std::string sidebarRoot_;
    SidebarNode sidebarTree_;
    std::atomic<bool> sidebarWatchRunning_{false};
    std::atomic<bool> sidebarRefreshPending_{false};
    std::vector<UndoStep> undoStack_, redoStack_;
    std::chrono::milliseconds undoWindow_{500};
    FindState find_;
    int findFocus_ = 0;
    GotoState goto_;
    enum GotoMode { Files, Symbols, Lines, Words } gotoMode_ = Files;
    void updateGotoResults();
    CommandPaletteState commandPalette_;
    void updateCommandPalette();
    void executePaletteCommand(int commandIndex);
    // autocomplete
    bool acActive_ = false;
    std::vector<std::string> acItems_;
    int acSelected_ = 0;
    std::string acPrefix_;
    size_t acPrefixStart_ = 0;
    void updateAutocomplete();
    void acceptAutocomplete();
    std::set<std::pair<int,int>> foldedRegions_;
    void toggleFold(size_t line);
    bool isFolded(size_t line) const;
    bool isFoldStart(size_t line) const;
    bool isFoldableLine(size_t line) const;
    size_t findFoldEnd(size_t startLine) const;
    void foldAll();
    void unfoldAll();
    bool pendingCtrlK_ = false;
    std::vector<int> lineIndents_;
    void computeLineIndents();
    bool useTabs_ = false;
    int tabSize_ = 2;
    struct UiColor { float r, g, b, a; };
    UiColor accentColor_{0.306f, 0.788f, 0.690f, 1.f};
    std::string gitBranch_;
    std::mutex gitBranchMutex_;
    std::atomic<bool> gitBranchBusy_{false};
    std::vector<std::string> closedTabStack_;
    bool closeConfirmOpen_ = false;
    size_t closeConfirmIndex_ = 0;
    std::string menuStyle_ = "icon";
    // popup window
    SDL_Window* popupWin_ = nullptr;
    float popupMainX_ = 0.f, popupMainY_ = 0.f, popupMainW_ = 0.f, popupMainH_ = 0.f;
    int popupScreenX_ = 0, popupScreenY_ = 0;
    bool deferPopupDraw_ = false;
    void ensurePopupWindow();
    void renderPopupToWindow(int x, int y, int w, int h);
    void hidePopupWindow();
    void convertIndentation(bool toSpaces);
    void guessIndent();
    void notifySyntaxEdit(size_t startByte, size_t oldEndByte, size_t newEndByte);
    StatusPopup statusPopup_ = StatusPopup::None;
    int popupSelected_ = 0;
    int popupScroll_ = 0;
    float popupX_ = 0, popupY_ = 0;
    int syntaxLangIndex_ = -1;
    static const char* syntaxLanguages[];
    static constexpr int syntaxLangCount = 44;
    Titlebar* titlebar_ = nullptr;
    Gutter* gutter_ = nullptr;
    Minimap* minimap_ = nullptr;
    StatusBar* statusbar_ = nullptr;
    SyntaxHighlighter* syntax_ = nullptr;
    friend SDL_HitTestResult hitTestCallback(SDL_Window*, const SDL_Point*, void*);
};
