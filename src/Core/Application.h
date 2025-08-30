#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <chrono>

class Titlebar;
class Gutter;
class Minimap;
class Rope;

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
    std::string query;
    bool regex = false;
    bool caseSensitive = false;
    std::vector<size_t> matches;
    size_t currentMatch = 0;
};

class Application {
public:
    static Application& instance();
    int run(int argc, char** argv);
    const AppPaths& paths() const { return paths_; }
    void quit() { running_ = false; }
private:
    Application() = default;
    bool init(int argc, char** argv);
    void shutdown();
    void initPaths();
    void handleEvents();
    void update();
    void render();
    void updateTitle();
    // buffer helpers
    size_t lineStart(size_t pos) const;
    size_t lineEnd(size_t pos) const;
    size_t lineStartForLine(size_t line) const;
    size_t lineOfPos(size_t pos) const;
    size_t totalLines() const;
    void insertAtCursor(const std::string& text);
    void deleteSelection();
    void insertText(const std::string& text);
    // selection helpers
    void findAllMatches();
    void ensureCursorVisible();
    // file ops
    void openFile(const std::string& path);
    void saveFile();
    void saveFileAs();
    void newBuffer();
    // undo
    void pushUndo();
    void doUndo();
    void doRedo();
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    AppPaths paths_;
    bool running_ = true;
    bool dirty_ = false;
    // buffer
    std::string textBuffer;
    std::string openFilePath_;
    std::string openFile_ = "untitled";
    // cursor / selection / multi-cursor
    std::vector<SelRange> selections_;
    float desiredCursorX_ = -1.f; // for vertical movement
    // scrolling
    float scrollY_ = 0.f;
    float maxScrollY_ = 0.f;
    // undo/redo
    std::vector<UndoStep> undoStack_, redoStack_;
    std::chrono::milliseconds undoWindow_{500};
    // find
    FindState find_;
    // UI
    Titlebar* titlebar_ = nullptr;
    Gutter* gutter_ = nullptr;
    Minimap* minimap_ = nullptr;
    friend SDL_HitTestResult hitTestCallback(SDL_Window*, const SDL_Point*, void*);
};
