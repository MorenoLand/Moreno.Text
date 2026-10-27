#pragma once
#include <functional>
#include "Core/SpellChecker.h"
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <set>
#include <memory>
#include "Plugin/PluginHost.h"
#include "UI/SidebarMenu.h"

class Titlebar;
class Gutter;
class Minimap;
class StatusBar;
class SyntaxHighlighter;
class FileBackedBuffer;

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
    std::vector<std::string> data;
    int selected = 0;
};

struct CommandPaletteState {
    bool active = false;
    std::string query;
    size_t cursor = 0;
    size_t anchor = 0;
    std::vector<int> results;
    int selected = 0;
    int scroll = 0;
    int hover = -1;
    bool scrollbarDragging = false;
    float scrollbarDragOffset = 0.f;
};

struct PluginPaletteCommand {
    std::string caption;
    std::string name;
};

struct PluginQuickPanelState {
    bool active = false;
    bool submitted = false;
    bool rich = false;
    std::string requestPath;
    std::string responsePath;
    std::vector<std::string> items;
    std::vector<std::vector<std::string>> details;
    std::vector<std::string> annotations;
    std::vector<std::string> linkTexts;
    std::vector<std::string> linkUrls;
    std::vector<int> results;
    std::string query;
    int selected = 0;
    int scroll = 0;
    bool scrollbarDragging = false;
    float scrollbarDragOffset = 0.f;
};

struct PluginInputPanelState {
    bool active = false;
    bool submitted = false;
    std::string requestPath;
    std::string responsePath;
    std::string caption;
    std::string text;
};

struct DiffHunk {
    size_t newStart, newEnd, oldStart, oldEnd;
};

struct SnippetDef {
    std::string content, trigger, description, scope;
};

struct EditorGroup {
    size_t tab = 0;
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
    bool binaryView = false;
    uintmax_t binaryFileSize = 0;
    uintmax_t binaryLoadedBytes = 0;
    bool preview = false;
    bool largeFileGuarded = false;
    uintmax_t largeFileSize = 0;
    size_t largeFileTotalLines = 0;
    size_t largeFileWindowFirstLine = 0;
    std::vector<uintmax_t> largeFileLineOffsets;
    uint64_t largeFileWindowStartByte = 0;
    uint64_t largeFileWindowEndByte = 0;
    std::shared_ptr<FileBackedBuffer> largeFileBuffer;
    float desiredCursorX = -1.f;
    std::string pluginSyntax;
    std::string pluginColorScheme;
    std::string manualSyntaxName;
    bool pluginOwned = false;
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
    int runSelfTest(const std::string& resultPath);
    bool isBorderlessMaximized() const { return borderlessMaximized_; }
    const AppPaths& paths() const { return paths_; }
    void quit() { running_ = false; }
    SyntaxHighlighter& syntax() { return *syntax_; }
    void newBuffer();
    void saveFile();
    void saveFileAs();
    void openFile(const std::string& path, bool preview = false);
    void openRecentFile(size_t index);
    const std::vector<std::string>& recentFiles() const { return recentFiles_; }
    void switchToTab(size_t index);
    void closeTab(size_t index);
    void closeCurrentTab() { closeTab(activeTab_); }
    void reopenClosedTab();
    bool detachTabToNewWindow(size_t index);
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
    void commandFind() { findPanelIsInFiles_ = false; find_.active = true; find_.replaceActive = false; find_.query.clear(); find_.matches.clear(); }
    void commandReplace() { findPanelIsInFiles_ = false; find_.active = true; find_.replaceActive = true; find_.query.clear(); find_.matches.clear(); }
    void commandGotoAnything() { promptActive_ = false; goto_.active = true; goto_.query.clear(); goto_.selected = 0; goto_.items.clear(); }
    void commandPalette() { commandPalette_.active = true; commandPalette_.query.clear(); commandPalette_.cursor = commandPalette_.anchor = 0; commandPalette_.selected = 0; commandPalette_.scroll = 0; commandPalette_.hover = -1; updateCommandPalette(); }
    void toggleConsole();
    void toggleFullscreen();
    void toggleMinimap() { minimapVisible_ = !minimapVisible_; }
    void toggleWordWrap() { wordWrap_ = !wordWrap_; visualLines_.clear(); }
    void toggleMaximize();
    void toggleBoldText();
    void openSettingsFile();
    void openKeyBindingsFile();
    void importVimSettings(bool neovim);
    void swapLineUp();
    void swapLineDown();
    void duplicateLine();
    void deleteLine();
    void joinLines();
    void toggleLineComment();
    void toggleBlockComment();
    void convertCaseUpper();
    void convertCaseLower();
    void convertCaseTitle();
    void convertCaseSwap();
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    void clearBookmarks();
    void indentSelection();
    void unindentSelection();
    void findNext();
    void findPrevious();
    void findAllSelect();
    void commandFindInFiles();
    void expandSelectionToLine();
    void expandSelectionToWord();
    void expandSelectionToBrackets();
    void expandSelectionToIndentation();
    void expandSelectionToScope();
    void splitSelectionIntoLines();
    void singleSelection();
    void toggleTabs();
    void toggleStatusBar();
    void toggleDistractionFree();
    void commandGotoSymbol() { openGotoWithPrefix("@"); }
    void commandGotoLine() { openGotoWithPrefix(":"); }
    void commandGotoWord() { openGotoWithPrefix("#"); }
    void jumpBack();
    void jumpForward();
    void openSyntaxPicker();
    void openIndentationPicker();
    void insertLineBefore();
    void insertLineAfter();
    void deleteWordBackward();
    void deleteWordForward();
    void deleteToLineEnd();
    void deleteToLineStart();
    void transposeCharacters();
    void sortLines(bool caseSensitive);
    void reverseLines();
    void uniqueLines();
    void addPreviousLine();
    void addNextLine();
    void changeFontSize(int delta);
    void resetFontSize();
    void newWindow();
    void closeAllFiles();
    void closeFolder();
    void refreshFolder();
    void openDocumentation();
    void reportBug();
    void showAbout();
    void foldCurrent();
    void unfoldCurrent();
    void commandFoldAll() { foldByLevel(1); }
    void commandUnfoldAll() { unfoldAll(); }
    void convertLineEndings(bool crlf);
    void nextTab();
    void previousTab();
    void selectTabByIndex(size_t index);
    void selectLastTab();
    void closeWindow() { running_ = false; }
    void findUnder(bool forward);
    void findAllUnder();
    void quickSkipNext();
    void useSelectionForFind();
    void useSelectionForReplace();
    void replaceNext();
    void setMark();
    void selectToMark();
    void deleteToMark();
    void swapWithMark();
    void yank();
    void showAtCenter();
    void wrapParagraph(size_t width);
    void jumpToMatchingBracket();
    void foldByLevel(int level);
    void showCompletions();
    void toggleMacroRecording();
    void playMacro();
    void saveMacro();
    bool macroRecording() const { return macroRecording_; }
    void nextResult();
    void insertNewline();
    void loadKeymaps();
    void registerCommandHandlers();
    void applyLiveSettings();
    bool matchBracketsEnabled() const { return matchBrackets_; }
    void applySaveTransforms();
    void noteFileTime(const std::string& path);
    void pollExternalChanges();
    void pressTab(bool shift);
    void switchHeaderImplementation();
    void cloneTab(size_t index);
    void diffWithTab(size_t otherIndex);
    void splitView() { cloneTab(activeTab_); }
    void sidebarContextAction(int action, const SidebarNode& node);
    void openPrompt(const std::string& caption, const std::string& initial, std::function<void(const std::string&)> callback);
    bool createPath(const std::string& dir, const std::string& name, bool folder, std::string& created);
    bool renamePath(const std::string& oldPath, const std::string& newName);
    bool deletePath(const std::string& path);
    void refreshSidebarTree();
    void toggleSpellCheck();
    void nextMisspelling();
    void previousMisspelling();
    void addWordToDictionary();
    void openSpellSuggestions();
    void openDictionaryPicker();
    void chooseDictionary(const std::string& path);
    void newPlugin();
    void newSnippet();
    void newSyntax();
    void newBuildSystem();
    void openMouseBindings();
    void browsePackages();
    void openPackageFilePicker();
    void copyAsHtml();
    void printDocument();
    std::string exportToHtml(bool wholeDocument);
    void nextModification();
    void previousModification();
    void revertHunk();
    void revertAllModifications();
    void openProject();
    void openProjectFile(const std::string& path);
    void saveProjectAs();
    void editProject();
    void closeProject();
    void openRecentProjectPicker();
    void openReopenEncodingPicker();
    void openSaveEncodingPicker();
    void reopenWithEncoding(const std::string& name);
    void saveWithEncoding(const std::string& name);
    void applyEncodingChoice(const std::string& data);
    void setRuler(int column);
    void setWrapColumn(int column);
    void softUndo();
    void softRedo();
    void openPasteHistory();
    void convertIdentifierCase(int style);
    void closeTag();
    void reindent();
    void shuffleLines();
    void permuteSelections(int mode);
    void trackSelectionHistory();
    void openSnippetPicker();
    void wrapSelectionWithTag();
    void insertSnippet(const std::string& content);
    bool advanceSnippet(bool forward);
    bool expandSnippetTrigger();
    bool snippetActive() const { return snippetActive_; }
    void endSnippet() { snippetActive_ = false; }
    void openColorSchemePicker();
    void customizeColorScheme();
    void applyColorScheme(const std::string& path);
    void setLayoutColumns(size_t columns);
    void focusGroup(size_t index);
    void focusNeighboringGroup(bool next);
    void moveFileToGroup(size_t index);
    void moveFileToNeighboringGroup(bool next);
    void newGroup();
    void closeGroup();
    struct BuildJob;
    void runBuild();
    void runBuildChoice(size_t choiceIndex);
    void cancelBuild();
    void openBuildPicker();
    void gotoDefinition();
    void gotoReference();
    void commandGotoSymbolInProject() { openGotoWithPrefix("@@"); }
    void openLocation(const std::string& path, size_t line);
    void previousResult();
    bool openFindResultAtCursor();
private:
    void openGotoWithPrefix(const std::string& prefix);
    void recordJump();
    void applyJump(size_t index);
    size_t matchLengthAt(size_t pos) const;
    void selectMatch(size_t pos);
    void runFindInFiles();
    bool findPanelIsInFiles_ = false;
    size_t markPos_ = std::string::npos;
    bool macroRecording_ = false, macroPlaying_ = false;
    std::vector<SDL_Event> macro_;
    void loadMacro();
    void indexFindResults();
    bool autoIndent_ = true, matchBrackets_ = true, matchSelection_ = true;
    std::map<std::string, std::filesystem::file_time_type> knownFileTimes_;
    uint32_t lastExternalPoll_ = 0;
    size_t matchingBracketOf(size_t at) const;
    void drawEditorHighlights(float drawTextX, float textOriginY, float lineStep, float top, float bottom, size_t firstLine, size_t lastLine, float gutterRight, float editorRight);
    bool handleCommandShortcut(SDL_Keycode sym, Uint16 mod);
    void insertTextMulti(const std::string& text, bool distributeLines);
    void deleteMulti(bool forward);
    void moveAllCursors(int key, bool extend);
    void refreshProjectFileList();
    void updateFileResults(const std::string& query);
    void openGotoFile(const std::string& data);
    std::vector<std::string> projectFiles_;
    std::string projectFilesRoot_;
    uint32_t projectFilesAt_ = 0;
    size_t gotoPendingLine_ = 0;
    void refreshSidebarNode(SidebarNode& node);
    bool promptActive_ = false;
    std::string promptCaption_, findScopeDir_, findResultsRoot_;
    std::function<void(const std::string&)> promptCallback_;
    bool ensureSpellDictionary();
    void loadSpellSetting();
    std::vector<std::pair<size_t, size_t>> misspelledInLine(size_t line) const;
    void updateSpellSuggestionResults();
    void applySpellSuggestion(const std::string& choice);
    void updateDictionaryResults(const std::string& pattern);
    SpellChecker spell_;
    bool spellCheck_ = false;
    size_t spellWordStart_ = 0, spellWordEnd_ = 0;
    void createFromTemplate(const std::string& stem, const std::string& extension, const std::string& contents);
    void updatePackageFileResults(const std::string& pattern);
    void refreshDiffHunks(bool force);
    void drawModificationMarkers(float gutterRight, float textOriginY, float lineStep, float top, float bottom);
    std::vector<DiffHunk> diffHunks_;
    std::vector<std::string> diffOldLines_;
    uint32_t diffComputedAt_ = 0;
    void updateEncodingResults(const std::string& query);
    void updateProjectResults(const std::string& pattern);
    std::string encodeForSave(const std::string& path, const std::string& utf8) const;
    std::map<std::string, std::string> fileEncodings_;
    std::string projectFile_;
    void rememberClipboardText(const std::string& text);
    void updatePasteHistoryResults(const std::string& pattern);
    std::vector<int> rulers_;
    int wrapColumn_ = 0;
    std::vector<std::string> clipHistory_;
    std::vector<std::vector<SelRange>> selectionHistory_, selectionRedo_;
    std::vector<SelRange> selectionSnapshot_;
    void loadSnippets();
    bool snippetScopeMatches(const std::string& scope) const;
    void updateSnippetResults(const std::string& pattern);
    void insertSnippetByIndex(size_t index);
    void selectSnippetStop();
    std::vector<SnippetDef> snippets_;
    uint32_t snippetsLoadedAt_ = 0;
    bool snippetActive_ = false;
    std::vector<std::vector<std::pair<size_t, size_t>>> snippetStops_;
    size_t snippetCurrent_ = 0, snippetFinal_ = 0, snippetBufSize_ = 0;
    void updateColorSchemeResults(const std::string& pattern);
    void loadSavedColorScheme();
    void startBuild(const nlohmann::json& spec, const std::string& label);
    void pollBuild();
    void appendToBuildTab(const std::string& chunk);
    void showBuildTab();
    void indexBuildResults();
    void updateProjectSymbolResults(const std::string& pattern);
    void updateBuildPickerResults(const std::string& pattern);
    std::shared_ptr<BuildJob> buildJob_;
    std::string buildSystemName_, buildFileRegex_, buildWorkDir_;
    std::vector<std::string> projectSymbolNames_, projectSymbolPaths_;
    std::vector<size_t> projectSymbolLines_;
    uint32_t projectSymbolsBuiltAt_ = 0;
    std::string projectSymbolsRoot_;
    void openFindHit(size_t index);
    std::vector<std::pair<std::string, size_t>> findHits_;
    std::vector<std::pair<size_t, size_t>> findResultLines_;  // buffer line -> index into findHits_
    size_t findHitCursor_ = 0;
    std::string macroPath() const;
    std::string killRing_;
    std::string wordOrSelectionAtCursor() const;
    bool tabsVisible_ = true, statusBarVisible_ = true, distractionFree_ = false;
    bool dfSavedSidebar_ = false, dfSavedMinimap_ = false, dfSavedTabs_ = true, dfSavedStatus_ = true, dfSavedFullscreen_ = false;
    std::vector<std::pair<size_t, size_t>> jumpStack_;
    size_t jumpIndex_ = 0;
    Application() = default;
    bool init(int argc, char** argv);
    void shutdown();
    void initPaths();
    void loadRecentFiles();
    void saveRecentFiles() const;
    void rememberRecentFile(const std::string& path);
    bool loadSession();
    void saveSession();
    void handleEvents();
    void update();
    void render();
    void updateTitle();
    void invalidateLineIndex() const { lineIndexDirty_ = true; }
    void rebuildLineIndex() const;
    size_t lineStart(size_t pos) const;
    size_t lineEnd(size_t pos) const;
    size_t lineStartForLine(size_t line) const;
    size_t lineOfPos(size_t pos) const;
    size_t totalLines() const;
    size_t colOfPos(size_t pos) const;
    void ensureLargeFileWindowForLine(size_t line);
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
    void refreshPluginCommands();
    void executePluginCommand(int index);
    // tabs
    std::vector<TabBuffer> tabs_;
    size_t activeTab_ = 0;
    std::vector<EditorGroup> editorGroups_;
    size_t activeGroup_ = 0;
    void saveCurrentTab();
    void loadTab(size_t index);
    void normalizeEditorGroups();
    void collapseEditorGroupsTo(size_t tabIndex);
    void addEditorGroupForTab(size_t tabIndex);
    void closeTabNow(size_t index);
    bool importDetachedTabFromFile(const std::string& path);
    void loadMoreBinaryText();
    bool detachTabToExistingWindow(size_t index);
    void drawTabBar(class FontAtlas& font, float windowW, float titlebarH);
    bool handleTabBarEvent(const SDL_Event& e, float windowW, float titlebarH);
    float tabBarH_ = 35.f;
    float tabChevronX_ = 0.f;
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    SDL_Cursor* sidebarResizeCursor_ = nullptr;
    SDL_Cursor* textCursor_ = nullptr;
    void* nativeWindowHandle_ = nullptr;
    AppPaths paths_;
    bool running_ = true, dirty_ = false, binaryView_ = false;
    uintmax_t binaryFileSize_ = 0, binaryLoadedBytes_ = 0;
    bool pluginHostStartPending_ = false;
    bool syntaxDirty_ = true;
    std::string textBuffer;
    mutable bool lineIndexDirty_ = true; mutable size_t lineIndexSize_ = 0;
    mutable std::vector<size_t> lineStarts_{0};
    std::string openFilePath_;
    std::string openFile_ = "untitled";
    std::vector<SelRange> selections_;
    float desiredCursorX_ = -1.f;
    float scrollY_ = 0.f;
    float scrollX_ = 0.f;
    float maxLineWidth_ = 0.f;
    bool maxLineWidthDirty_ = true;
    bool largeFileGuarded_ = false;
    uintmax_t largeFileSize_ = 0;
    size_t largeFileTotalLines_ = 0;
    size_t largeFileWindowFirstLine_ = 0;
    std::vector<uintmax_t> largeFileLineOffsets_;
    uint64_t largeFileWindowStartByte_ = 0;
    uint64_t largeFileWindowEndByte_ = 0;
    std::shared_ptr<FileBackedBuffer> largeFileBuffer_;
    bool selecting_ = false;
    bool minimapDragging_ = false;
    bool minimapPendingJump_ = false;
    float minimapDragStartY_ = 0.f;
    float minimapDragStartScrollY_ = 0.f;
    float minimapPendingJumpY_ = 0.f;
    bool minimapVisible_ = true;
    bool fullscreen_ = false;
    bool borderlessMaximized_ = false;
    int restoreWindowX_ = 0, restoreWindowY_ = 0, restoreWindowW_ = 0, restoreWindowH_ = 0;
    bool scrollbarHovered_ = false;
    bool scrollbarDragging_ = false;
    bool horizontalScrollbarHovered_ = false;
    bool horizontalScrollbarDragging_ = false;
    float scrollbarDragOffset_ = 0.f;
    float horizontalScrollbarDragOffset_ = 0.f;
    int mouseX_ = 0, mouseY_ = 0;
    bool autoScrollActive_ = false;
    float autoScrollAnchorX_ = 0.f, autoScrollAnchorY_ = 0.f;
    uint32_t autoScrollLastTick_ = 0;
    float tabScrollX_ = 0.f;
    bool tabDropdownOpen_ = false;
    int tabDropdownHover_ = -1;
    bool tabContextOpen_ = false;
    int tabContextHover_ = -1;
    size_t tabContextIndex_ = 0;
    float tabContextX_ = 0.f, tabContextY_ = 0.f;
    bool sidebarVisible_ = false;
    bool sidebarResizing_ = false;
    bool sidebarScrollbarDragging_ = false;
    bool sidebarHScrollbarDragging_ = false;
    float sidebarScrollbarDragStartY_ = 0.f;
    float sidebarScrollbarDragStartScrollY_ = 0.f;
    float sidebarHScrollbarDragStartX_ = 0.f;
    float sidebarHScrollbarDragStartScrollX_ = 0.f;
    float sidebarWidth_ = 200.f;
    float sidebarScrollY_ = 0.f;
    float sidebarScrollX_ = 0.f;
    float sidebarContentH_ = 0.f;
    float sidebarContentW_ = 0.f;
    std::string sidebarRoot_;
    SidebarNode sidebarTree_;
    bool sidebarContextOpen_ = false;
    int sidebarContextHover_ = -1;
    float sidebarContextX_ = 0.f, sidebarContextY_ = 0.f;
    SidebarNode sidebarContextNode_;
    std::vector<SidebarMenuItem> sidebarContextItems_;
    std::atomic<bool> sidebarWatchRunning_{false};
    std::atomic<bool> sidebarRefreshPending_{false};
    std::vector<UndoStep> undoStack_, redoStack_;
    std::chrono::milliseconds undoWindow_{500};
    FindState find_;
    int findFocus_ = 0;
    GotoState goto_;
    enum GotoMode { Files, Symbols, Lines, Words, ProjectSymbols, Builds, ColorSchemes, Snippets, PasteHistory, Encodings, Projects, PackageFiles, SpellSuggest, Dictionaries } gotoMode_ = Files;
    void updateGotoResults();
    CommandPaletteState commandPalette_;
    std::string pluginStatusText_;
    uint32_t pluginStatusUntil_ = 0;
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
    std::string pendingChordKey_;
    // bookmarks
    std::set<int> bookmarks_;
    // split view
    bool splitActive_ = false;
    size_t splitTab_ = 0;
    float splitDividerPos_ = 0.5f;
    bool splitDragging_ = false;
    bool splitFocusRight_ = false;
    float splitScrollY_ = 0.f;
    std::vector<SelRange> splitSelections_;
    // diff view
    bool diffActive_ = false;
    std::vector<std::pair<size_t,size_t>> diffAdded_, diffRemoved_, diffChanged_;
    // tab drag
    bool tabDragging_ = false, tabDragPending_ = false;
    size_t tabDragIndex_ = 0;
    size_t tabHoverIndex_ = (size_t)-1;
    float tabDragStartX_ = 0.f, tabDragStartY_ = 0.f;
    bool tabDropPreview_ = false;
    uint32_t tabDropPreviewTick_ = 0;
    std::string tabDropPreviewLabel_;
    float tabDropPreviewX_ = -1.f;
    float tabDragOffsetX_ = 0.f;
    // word wrap
    bool wordWrap_ = false;
    struct VisualLine { int bufferLine; int startCol; int endCol; };
    std::vector<VisualLine> visualLines_;
    void buildVisualLineMap(float editorWidth, float gutterWidth);
    // box/column selection
    bool boxSelActive_ = false;
    int boxSelStartLine_ = 0, boxSelStartCol_ = 0;
    int boxSelEndLine_ = 0, boxSelEndCol_ = 0;
    // auto pair
    bool autoPair_ = true;
    bool scrollPastEnd_ = true;
    bool inOsDialog_ = false;
    std::vector<int> lineIndents_;
    bool indentsDirty_ = true;
    std::vector<float> solidVerts_;
    bool debugFpsVisible_ = false;
    uint64_t fpsLastCounter_ = 0;
    float fpsSmoothedMs_ = 0.f;
    uint32_t cursorLastInputTicks_ = 0;
    bool consoleOpen_ = false;
    std::string consoleBuffer_;
    float consoleScrollY_ = 0.f;
    float consoleH_ = 150.f;
    bool consoleResizing_ = false;
    size_t consoleLogReadPos_ = 0;
    void pollConsoleLog();
    PluginQuickPanelState pluginQuickPanel_;
    PluginInputPanelState pluginInputPanel_;
    std::set<std::string> handledPluginRequests_;
    std::set<int> closedPluginViewIds_;
    std::string pendingPluginCommand_;
    uint32_t pendingPluginCommandTicks_ = 0;
    void pollPluginBridge();
    void finishPluginQuickPanel(int index);
    void finishPluginInputPanel(bool accepted);
    bool sendPluginViewTextCommand(const std::string& commandName, const std::string& characters);
    bool sendPluginViewWindowCommand(const std::string& commandName);
    void notifyPluginViewClosed(int id);
    bool writePluginViewCommandRequest(const std::string& type, const std::string& commandName, const std::string& characters);
    void computeLineIndents();
    void computeMaxLineWidth();
    bool useTabs_ = false;
    int tabSize_ = 2;
    int fontSize_ = 12;
    int sidebarFontSize_ = 12;
    int tabFontSize_ = 12;
    bool fontBold_ = true;
    struct UiColor { float r, g, b, a; };
    UiColor accentColor_{0.306f, 0.788f, 0.690f, 1.f};
    std::string gitBranch_;
    std::mutex gitBranchMutex_;
    std::atomic<bool> gitBranchBusy_{false};
    std::vector<std::string> closedTabStack_;
    std::vector<std::string> recentFiles_;
    std::vector<PluginPaletteCommand> pluginCommands_;
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
    void shapePopupWindowForMenu();
    void clearPopupWindowShape();
    std::string commentToken() const;
    void ensureWordSel();
    // auto pair
    void handleAutoPair(const char* text);
    void convertIndentation(bool toSpaces);
    void closeAllPopups();
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
