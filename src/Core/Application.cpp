#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "UI/Titlebar.h"
#include "UI/MenuBar.h"
#include "Settings/SettingsManager.h"
#include "Theme/ThemeEngine.h"
#include "Commands/KeyBindingManager.h"
#include "UI/Gutter.h"
#include "UI/Minimap.h"
#include "UI/StatusBar.h"
#include "Syntax/SyntaxHighlighter.h"
#include "Platform/Platform.h"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <GL/glew.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <chrono>
#include <cctype>
#include <thread>
#include <cmath>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

namespace fs = std::filesystem;

static SDL_HitTestResult hitTestCallback(SDL_Window*, const SDL_Point* area, void* userdata) {
    auto* app = static_cast<Application*>(userdata);
    return app->titlebar_ ? app->titlebar_->hitTest(area->x, area->y, app->window_) : SDL_HITTEST_NORMAL;
}

Application& Application::instance() { static Application app; return app; }

int Application::run(int argc, char** argv) {
    if (!init(argc, argv)) return 1;
    while (running_) { handleEvents(); update(); render(); }
    shutdown();
    return 0;
}

static std::string findMonospaceFont() {
    static const char* candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/consola.ttf","C:/Windows/Fonts/cour.ttf","C:/Windows/Fonts/lucon.ttf",
#elif __APPLE__
        "/System/Library/Fonts/SFNSMono.ttf","/System/Library/Fonts/Menlo.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
#endif
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) if (fs::exists(candidates[i])) return candidates[i];
    return "";
}

bool Application::init(int argc, char** argv) {
    initPaths();
    // load settings
    {
        std::string userDir = paths_.dataDir + "/Packages/User";
        fs::create_directories(userDir);
        std::string settingsPath = userDir + "/Preferences.moreno-settings";
        auto& settings = SettingsManager::instance();
        settings.load(settingsPath);
        settings.ensureDefaults();
        settings.save();
        tabSize_ = settings.get<int>("tab_size", 4);
        useTabs_ = !settings.get<bool>("translate_tabs_to_spaces", true);
        autoPair_ = settings.get<bool>("auto_match_enabled", true);
        scrollPastEnd_ = settings.get<bool>("scroll_past_end", true);
        minimapVisible_ = settings.get<bool>("minimap_visible", true);
    }
    // load color scheme
    {
        std::string defaultDir = paths_.dataDir + "/Packages/Default";
        fs::create_directories(defaultDir);
        std::string schemePath = defaultDir + "/Moreno Dark.moreno-color-scheme";
        auto& theme = ThemeEngine::instance();
        if (!fs::exists(schemePath)) theme.writeDefault(schemePath);
        theme.load(schemePath);
    }
    // load keybindings
    {
        std::string defaultDir = paths_.dataDir + "/Packages/Default";
        std::string keymapPath = defaultDir + "/Default (Windows).moreno-keymap";
        auto& kb = KeyBindingManager::instance();
        if (!fs::exists(keymapPath)) kb.writeDefault(keymapPath);
        kb.load(keymapPath);
        // user overrides
        std::string userDir = paths_.dataDir + "/Packages/User";
        std::string userKeymap = userDir + "/Default (Windows).moreno-keymap";
        if (fs::exists(userKeymap)) kb.load(userKeymap);
        kb.addCommandHandler("new_file", [this](){ newBuffer(); });
        kb.addCommandHandler("open_file", [this](){ openFileDialog(); });
        kb.addCommandHandler("save", [this](){ saveFile(); });
        kb.addCommandHandler("save_as", [this](){ saveFileAs(); });
        kb.addCommandHandler("close_file", [this](){ closeCurrentTab(); });
        kb.addCommandHandler("reopen_closed_tab", [this](){ reopenClosedTab(); });
        kb.addCommandHandler("undo", [this](){ doUndo(); });
        kb.addCommandHandler("redo", [this](){ doRedo(); });
        kb.addCommandHandler("cut", [this](){ cutSelectionOrLine(); });
        kb.addCommandHandler("copy", [this](){ copySelectionOrLine(); });
        kb.addCommandHandler("paste", [this](){ pasteClipboard(false); });
        kb.addCommandHandler("paste_and_indent", [this](){ pasteClipboard(true); });
        kb.addCommandHandler("select_all", [this](){ commandSelectAll(); });
        kb.addCommandHandler("find", [this](){ commandFind(); });
        kb.addCommandHandler("replace", [this](){ commandReplace(); });
        kb.addCommandHandler("goto_anything", [this](){ commandGotoAnything(); });
        kb.addCommandHandler("command_palette", [this](){ commandPalette(); });
        kb.addCommandHandler("find_under_expand", [this](){ /* Ctrl+D next occurrence */ });
        kb.addCommandHandler("select_line", [this](){ /* Ctrl+L */ });
        kb.addCommandHandler("split_selection_into_lines", [this](){ /* Ctrl+Shift+L */ });
        kb.addCommandHandler("toggle_comment", [this](){ toggleLineComment(); });
        kb.addCommandHandler("toggle_block_comment", [this](){ toggleBlockComment(); });
        kb.addCommandHandler("swap_line_up", [this](){ swapLineUp(); });
        kb.addCommandHandler("swap_line_down", [this](){ swapLineDown(); });
        kb.addCommandHandler("duplicate_line", [this](){ duplicateLine(); });
        kb.addCommandHandler("delete_line", [this](){ deleteLine(); });
        kb.addCommandHandler("join_lines", [this](){ joinLines(); });
        kb.addCommandHandler("upper_case", [this](){ convertCaseUpper(); });
        kb.addCommandHandler("lower_case", [this](){ convertCaseLower(); });
        kb.addCommandHandler("toggle_full_screen", [this](){ toggleFullscreen(); });
        kb.addCommandHandler("toggle_word_wrap", [this](){ toggleWordWrap(); });
        kb.addCommandHandler("toggle_bookmark", [this](){ toggleBookmark(); });
        kb.addCommandHandler("next_bookmark", [this](){ nextBookmark(); });
        kb.addCommandHandler("prev_bookmark", [this](){ prevBookmark(); });
        kb.addCommandHandler("clear_bookmarks", [this](){ clearBookmarks(); });
        kb.addCommandHandler("toggle_side_bar", [this](){ toggleSidebar(); });
    }
    selections_.emplace_back(0);
    TabBuffer initTab; initTab.fileName = "untitled"; initTab.selections.emplace_back(0);
    tabs_.push_back(std::move(initTab));
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return false; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    window_ = SDL_CreateWindow("Moreno Text", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS);
    if (!window_) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }
    SDL_SetWindowResizable(window_, SDL_TRUE);
    SDL_SetWindowMinimumSize(window_, 400, 300);
    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);
    glewExperimental = GL_TRUE;
    if (GLenum err = glewInit(); err != GLEW_OK) { fprintf(stderr, "glewInit: %s\n", glewGetErrorString(err)); return false; }
    int w, h;
    SDL_GL_GetDrawableSize(window_, &w, &h);
    if (!GLRenderer::init(w, h)) return false;
    std::string fontPath = findMonospaceFont();
    if (fontPath.empty() || !fontAtlas().init(fontPath)) { fprintf(stderr, "Font init failed\n"); return false; }
    titlebar_ = new Titlebar(); titlebar_->init(w);
    gutter_ = new Gutter();
    minimap_ = new Minimap();
    statusbar_ = new StatusBar();
    syntax_ = new SyntaxHighlighter();
    fs::create_directories(paths_.localDir);
    loadRecentFiles();
    // window icon
    std::string iconPath = (fs::path(paths_.exeDir) / "Assets" / "moreno_text.ico").string();
    if (!fs::exists(iconPath)) iconPath = (fs::path(paths_.exeDir) / ".." / "Assets" / "moreno_text.ico").string();
    if (fs::exists(iconPath)) {
        SDL_RWops* rw = SDL_RWFromFile(iconPath.c_str(), "rb");
        if (rw) { SDL_Surface* icon = SDL_LoadBMP_RW(rw, 1); if (icon) { SDL_SetWindowIcon(window_, icon); SDL_FreeSurface(icon); } }
    }
    SDL_SetWindowHitTest(window_, hitTestCallback, this);
    SDL_SysWMinfo wmInfo; SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window_, &wmInfo)) {
#ifdef _WIN32
        Platform::setRoundedCorners(reinterpret_cast<void*>(wmInfo.info.win.window), 8);
#endif
    }
    SDL_StartTextInput();
    if (argc > 1 && fs::exists(argv[1])) openFile(argv[1]);
    else if (!loadSession()) newBuffer();
    return true;
}

void Application::initPaths() {
#ifdef _WIN32
    wchar_t ep[MAX_PATH]; GetModuleFileNameW(nullptr, ep, MAX_PATH);
    paths_.exeDir = fs::path(ep).parent_path().string();
#else
    paths_.exeDir = fs::canonical("/proc/self/exe").parent_path().string();
#endif
    fs::path base(paths_.exeDir), dp = base / "Data";
    if (fs::exists(dp)) {
        paths_.portable = true; paths_.dataDir = dp.string();
        paths_.packagesDir = (dp / "Packages").string(); paths_.installedPackagesDir = (dp / "Installed Packages").string();
        paths_.cacheDir = (dp / "Cache").string(); paths_.localDir = (dp / "Local").string();
        paths_.libDir = (dp / "Lib").string(); paths_.backupDir = (dp / "Backup").string();
    } else {
        std::string ch;
#ifdef _WIN32
        ch = std::getenv("APPDATA");
#elif __APPLE__
        ch = std::string(std::getenv("HOME")) + "/Library/Application Support";
#else
        const char* xdg = std::getenv("XDG_CONFIG_HOME"); ch = xdg ? xdg : std::string(std::getenv("HOME")) + "/.config";
#endif
        fs::path p(ch); paths_.dataDir = (p / "Moreno Text").string();
        paths_.packagesDir = (p / "Moreno Text" / "Packages").string(); paths_.installedPackagesDir = (p / "Moreno Text" / "Installed Packages").string();
        paths_.cacheDir = (p / "Moreno Text" / "Cache").string(); paths_.localDir = (p / "Moreno Text" / "Local").string();
        paths_.libDir = (p / "Moreno Text" / "Lib").string(); paths_.backupDir = (p / "Moreno Text" / "Backup").string();
    }
}

// ── buffer helpers ──

void Application::loadRecentFiles() {
    recentFiles_.clear();
    std::ifstream f(fs::path(paths_.localDir) / "recent_files.json");
    if (!f) return;
    try {
        auto data = nlohmann::json::parse(f);
        if (!data.is_array()) return;
        for (auto& item : data) {
            if (!item.is_string()) continue;
            std::string path = item.get<std::string>();
            if (path.empty() || !fs::exists(path)) continue;
            if (std::find(recentFiles_.begin(), recentFiles_.end(), path) == recentFiles_.end()) recentFiles_.push_back(path);
            if (recentFiles_.size() >= 10) break;
        }
    } catch (...) {}
}

void Application::saveRecentFiles() const {
    try {
        fs::create_directories(paths_.localDir);
        nlohmann::json data = nlohmann::json::array();
        for (auto& path : recentFiles_) data.push_back(path);
        std::ofstream f(fs::path(paths_.localDir) / "recent_files.json", std::ios::binary);
        if (f) f << data.dump(2);
    } catch (...) {}
}

void Application::rememberRecentFile(const std::string& path) {
    if (path.empty()) return;
    std::string absPath;
    try { absPath = fs::absolute(path).string(); } catch (...) { absPath = path; }
    recentFiles_.erase(std::remove(recentFiles_.begin(), recentFiles_.end(), absPath), recentFiles_.end());
    recentFiles_.insert(recentFiles_.begin(), absPath);
    if (recentFiles_.size() > 10) recentFiles_.resize(10);
    saveRecentFiles();
}

void Application::openRecentFile(size_t index) {
    if (index >= recentFiles_.size()) return;
    std::string path = recentFiles_[index];
    if (!fs::exists(path)) {
        recentFiles_.erase(recentFiles_.begin() + index);
        saveRecentFiles();
        return;
    }
    openFile(path);
}

bool Application::loadSession() {
    std::ifstream f(fs::path(paths_.localDir) / "session.json");
    if (!f) return false;
    try {
        auto data = nlohmann::json::parse(f);
        if (!data.is_object() || !data.contains("tabs") || !data["tabs"].is_array()) return false;
        tabs_.clear();
        size_t restoreActive = data.value("active_tab", 0);
        for (auto& item : data["tabs"]) {
            if (!item.is_object()) continue;
            std::string path = item.value("path", "");
            if (path.empty() || !fs::exists(path)) continue;
            std::ifstream tf(path, std::ios::binary);
            if (!tf) continue;
            std::ostringstream ss; ss << tf.rdbuf();
            TabBuffer tab;
            tab.text = ss.str();
            tab.filePath = path;
            tab.fileName = fs::path(path).filename().string();
            tab.scrollY = item.value("scrollY", 0.f);
            size_t cursor = item.value("cursor", 0ull);
            if (cursor > tab.text.size()) cursor = tab.text.size();
            tab.selections.emplace_back(cursor);
            tabs_.push_back(std::move(tab));
            rememberRecentFile(path);
        }
        if (tabs_.empty()) return false;
        if (restoreActive >= tabs_.size()) restoreActive = tabs_.size() - 1;
        loadTab(restoreActive);
        return true;
    } catch (...) {
        return false;
    }
}

void Application::saveSession() {
    saveCurrentTab();
    try {
        fs::create_directories(paths_.localDir);
        nlohmann::json data;
        data["active_tab"] = activeTab_;
        data["tabs"] = nlohmann::json::array();
        for (auto& tab : tabs_) {
            if (tab.filePath.empty()) continue;
            nlohmann::json item;
            item["path"] = tab.filePath;
            item["scrollY"] = tab.scrollY;
            item["cursor"] = tab.selections.empty() ? 0 : tab.selections[0].cursor;
            data["tabs"].push_back(item);
        }
        std::ofstream f(fs::path(paths_.localDir) / "session.json", std::ios::binary);
        if (f) f << data.dump(2);
    } catch (...) {}
}

size_t Application::lineStart(size_t pos) const { if (pos == 0) return 0; size_t p = textBuffer.rfind('\n', pos - 1); return p == std::string::npos ? 0 : p + 1; }
size_t Application::lineEnd(size_t pos) const { size_t p = textBuffer.find('\n', pos); return p == std::string::npos ? textBuffer.size() : p; }
size_t Application::lineStartForLine(size_t line) const {
    size_t l = 0, pos = 0;
    while (l < line && pos < textBuffer.size()) { size_t nl = textBuffer.find('\n', pos); if (nl == std::string::npos) break; pos = nl + 1; ++l; }
    return pos;
}
size_t Application::lineOfPos(size_t pos) const { size_t line = 0; for (size_t i = 0; i < pos && i < textBuffer.size(); ++i) if (textBuffer[i] == '\n') ++line; return line; }
size_t Application::totalLines() const { if (textBuffer.empty()) return 1; size_t n = 1; for (char c : textBuffer) if (c == '\n') ++n; return n; }
size_t Application::colOfPos(size_t pos) const { size_t ls = lineStart(pos); return pos - ls; }

void Application::insertText(const std::string& text) {
    pushUndo();
    size_t oldPos = selections_[0].cursor;
    if (selections_[0].hasSelection()) deleteSelection();
    size_t pos = selections_[0].cursor;
    size_t startRow = lineOfPos(pos), startCol = pos - lineStart(pos);
    textBuffer.insert(pos, text);
    size_t newPos = pos + text.size();
    size_t newEndRow = lineOfPos(newPos), newEndCol = newPos - lineStart(newPos);
    syntax_->notifyEdit(pos, pos, newPos, startRow, startCol, startRow, startCol, newEndRow, newEndCol);
    selections_[0].anchor = selections_[0].cursor = newPos;
    desiredCursorX_ = -1.f; dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true; maxLineWidthDirty_ = true;
    // trigger autocomplete
    size_t wStart = pos;
    while (wStart > 0 && (isalnum(textBuffer[wStart-1]) || textBuffer[wStart-1] == '_')) --wStart;
    acPrefix_ = textBuffer.substr(wStart, newPos - wStart);
    acPrefixStart_ = wStart;
    updateAutocomplete();
}
void Application::insertAtCursor(const std::string& text) { insertText(text); }
void Application::deleteSelection() {
    if (!selections_[0].hasSelection()) return;
    size_t a = selections_[0].min(), b = selections_[0].max();
    size_t startRow = lineOfPos(a), startCol = a - lineStart(a);
    size_t oldEndRow = lineOfPos(b), oldEndCol = b - lineStart(b);
    textBuffer.erase(a, b - a);
    syntax_->notifyEdit(a, b, a, startRow, startCol, oldEndRow, oldEndCol, startRow, startCol);
    selections_[0].anchor = selections_[0].cursor = a;
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true; maxLineWidthDirty_ = true;
}

std::string Application::selectedTextOrLine() const {
    if (!selections_.empty() && selections_[0].hasSelection()) {
        size_t a = selections_[0].min(), b = selections_[0].max();
        return textBuffer.substr(a, b - a);
    }
    size_t ls = lineStart(selections_[0].cursor), le = lineEnd(selections_[0].cursor);
    if (le < textBuffer.size()) ++le;
    return textBuffer.substr(ls, le - ls);
}

void Application::copySelectionOrLine() {
    std::string text = selectedTextOrLine();
    if (!text.empty()) SDL_SetClipboardText(text.c_str());
}

void Application::cutSelectionOrLine() {
    copySelectionOrLine();
    pushUndo();
    if (selections_[0].hasSelection()) deleteSelection();
    else {
        size_t ls = lineStart(selections_[0].cursor), le = lineEnd(selections_[0].cursor);
        if (le < textBuffer.size()) ++le;
        textBuffer.erase(ls, le - ls);
        selections_[0].anchor = selections_[0].cursor = ls < textBuffer.size() ? ls : textBuffer.size();
        dirty_ = true;
    }
}

void Application::pasteClipboard(bool andIndent) {
    char* clip = SDL_GetClipboardText();
    if (!clip) return;
    std::string text(clip);
    SDL_free(clip);
    if (text.empty()) return;
    if (andIndent) {
        size_t ls = lineStart(selections_[0].cursor);
        std::string indent;
        while (ls + indent.size() < textBuffer.size() && (textBuffer[ls + indent.size()] == ' ' || textBuffer[ls + indent.size()] == '\t')) indent += textBuffer[ls + indent.size()];
        std::string adjusted; adjusted.reserve(text.size() + indent.size() * 4);
        for (size_t i = 0; i < text.size(); ++i) {
            adjusted += text[i];
            if (text[i] == '\n' && i + 1 < text.size()) adjusted += indent;
        }
        text = std::move(adjusted);
    }
    insertText(text);
}

void Application::ensureCursorVisible() {
    size_t curLine = lineOfPos(selections_[0].cursor);
    int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
    float tbH = titlebar_->height() + tabBarH_;
    float lineStep = fontAtlas().lineHeight();
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    float cursorY = textOriginY + curLine * lineStep - scrollY_;
    if (cursorY < textOriginY) scrollY_ -= (textOriginY - cursorY);
    else if (cursorY + lineStep > wh - statusbar_->height()) scrollY_ += (cursorY + lineStep - (wh - statusbar_->height()));
    if (scrollY_ < 0) scrollY_ = 0;
}

void Application::findAllMatches() {
    find_.matches.clear(); find_.currentMatch = 0;
    if (find_.query.empty()) return;
    if (find_.regex) {
        try {
            std::regex re(find_.query, find_.caseSensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase));
            for (auto it = std::sregex_iterator(textBuffer.begin(), textBuffer.end(), re); it != std::sregex_iterator(); ++it)
                find_.matches.push_back((size_t)it->position());
        } catch (...) {}
    } else {
        std::string hay = find_.caseSensitive ? textBuffer : std::string(textBuffer);
        std::string needle = find_.caseSensitive ? find_.query : std::string(find_.query);
        if (!find_.caseSensitive) {
            auto tl = [](std::string& s) { for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); };
            tl(hay); tl(needle);
        }
        size_t pos = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos) {
            if (find_.wholeWord) {
                bool lw = (pos == 0 || !isalnum(textBuffer[pos-1]));
                bool rw = (pos + needle.size() >= textBuffer.size() || !isalnum(textBuffer[pos + needle.size()]));
                if (!lw || !rw) { pos += needle.size(); continue; }
            }
            find_.matches.push_back(pos); pos += needle.size();
        }
    }
}

void Application::doReplace() {
    if (find_.currentMatch < find_.matches.size()) {
        pushUndo();
        size_t m = find_.matches[find_.currentMatch];
        textBuffer.replace(m, find_.query.size(), find_.replace);
        selections_[0].anchor = selections_[0].cursor = m + find_.replace.size();
        dirty_ = true;
        findAllMatches();
    }
}
void Application::doReplaceAll() {
    if (find_.matches.empty()) return;
    pushUndo();
    for (int i = (int)find_.matches.size() - 1; i >= 0; --i)
        textBuffer.replace(find_.matches[i], find_.query.size(), find_.replace);
    dirty_ = true;
    findAllMatches();
}

void Application::detectSyntax() {
    if (openFilePath_.empty()) { syntax_->setLanguage(""); syntaxDirty_ = true; indentsDirty_ = true; return; }
    std::string ext = fs::path(openFilePath_).extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    syntax_->setLanguage(ext);
    syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::updateGitBranch() {
    std::string dir;
    if (!sidebarRoot_.empty()) dir = sidebarRoot_;
    else if (!openFilePath_.empty()) dir = fs::path(openFilePath_).parent_path().string();
    if (dir.empty()) { std::lock_guard<std::mutex> lock(gitBranchMutex_); gitBranch_.clear(); return; }
    if (gitBranchBusy_.exchange(true)) return;
    std::thread([this, dir] {

#ifdef _WIN32
        std::string command = "git -C \"" + dir + "\" rev-parse --abbrev-ref HEAD 2>NUL";
        FILE* pipe = _popen(command.c_str(), "r");
#else
        std::string command = "git -C \"" + dir + "\" rev-parse --abbrev-ref HEAD 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
#endif
        if (!pipe) { std::lock_guard<std::mutex> lock(gitBranchMutex_); gitBranch_.clear(); gitBranchBusy_ = false; return; }

        char buf[256] = {};
        std::string branch;
        while (fgets(buf, sizeof(buf), pipe)) branch += buf;
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r' || branch.back() == ' ' || branch.back() == '\t')) branch.pop_back();
        { std::lock_guard<std::mutex> lock(gitBranchMutex_); gitBranch_ = (branch.empty() || branch == "HEAD") ? "" : branch; }
        gitBranchBusy_ = false;
    }).detach();
}

// ── file ops ──

void Application::openFile(const std::string& path) {
    saveCurrentTab();
    std::ifstream f(path, std::ios::binary); if (!f) return;
    std::ostringstream ss; ss << f.rdbuf();
    std::string absPath;
    try { absPath = fs::absolute(path).string(); } catch (...) { absPath = path; }
    TabBuffer tab; tab.text = ss.str(); tab.filePath = absPath; tab.fileName = fs::path(absPath).filename().string();
    tab.selections.emplace_back(tab.text.size());
    tabs_.push_back(std::move(tab)); activeTab_ = tabs_.size()-1;
    textBuffer = tabs_[activeTab_].text; openFilePath_ = absPath; openFile_ = tabs_[activeTab_].fileName;
    dirty_ = false; selections_.clear(); selections_.emplace_back(textBuffer.size());
    scrollY_ = 0; undoStack_.clear(); redoStack_.clear(); foldedRegions_.clear(); detectSyntax(); updateGitBranch();
    rememberRecentFile(absPath);
}
void Application::saveFile() { if (openFilePath_.empty()) { saveFileAs(); return; } std::ofstream f(openFilePath_, std::ios::binary); if (!f) return; f << textBuffer; dirty_ = false; rememberRecentFile(openFilePath_); updateGitBranch(); }
void Application::saveFileAs() {
#ifdef _WIN32
    closeAllPopups();
    inOsDialog_ = true;
    char buf[MAX_PATH] = {}; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "All Files\0*.*\0Text Files\0*.txt\0"; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "txt";
    if (GetSaveFileNameA(&ofn)) { try { openFilePath_ = fs::absolute(buf).string(); } catch (...) { openFilePath_ = buf; } openFile_ = fs::path(openFilePath_).filename().string(); detectSyntax(); saveFile(); updateGitBranch(); }
    inOsDialog_ = false;
#endif
}
void Application::saveAll() {
    saveCurrentTab();
    for (auto& tab : tabs_) {
        if (tab.filePath.empty() || !tab.dirty) continue;
        std::ofstream f(tab.filePath, std::ios::binary);
        if (f) { f << tab.text; tab.dirty = false; rememberRecentFile(tab.filePath); }
    }
    loadTab(activeTab_);
}
void Application::revertFile() {
    if (openFilePath_.empty()) return;
    std::ifstream f(openFilePath_, std::ios::binary);
    if (!f) return;
    std::ostringstream ss; ss << f.rdbuf();
    textBuffer = ss.str(); dirty_ = false; selections_.clear(); selections_.emplace_back(std::min(selections_.empty() ? 0 : selections_[0].cursor, textBuffer.size()));
}
void Application::openFileDialog() {
#ifdef _WIN32
    closeAllPopups();
    inOsDialog_ = true;
    char buf[MAX_PATH] = {}; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "All Files\0*.*\0"; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) openFile(buf);
    inOsDialog_ = false;
#endif
}
void Application::newBuffer() {
    saveCurrentTab();
    TabBuffer tab; tab.fileName = "untitled"; tab.selections.emplace_back(0);
    tabs_.push_back(std::move(tab)); activeTab_ = tabs_.size()-1;
    textBuffer.clear(); openFilePath_.clear(); openFile_ = "untitled"; dirty_ = false;
    selections_.clear(); selections_.emplace_back(0); scrollY_ = 0; undoStack_.clear(); redoStack_.clear(); foldedRegions_.clear(); detectSyntax();
}

void Application::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Application::openFolderDialog() {
    closeAllPopups();
    std::string path;
    inOsDialog_ = true;
    if (Platform::pickFolder(path)) loadFolder(path);
    inOsDialog_ = false;
}

void Application::loadFolder(const std::string& path) {
    sidebarRoot_ = path;
    sidebarTree_ = {};
    sidebarTree_.name = fs::path(path).filename().string();
    if (sidebarTree_.name.empty()) sidebarTree_.name = path;
    sidebarTree_.path = path;
    sidebarTree_.folder = true;
    sidebarTree_.expanded = true;
    buildSidebarNode(sidebarTree_);
    sidebarVisible_ = true;
    updateGitBranch();
#ifdef _WIN32
    sidebarWatchRunning_ = false;
    sidebarWatchRunning_ = true;
    std::thread([this, path] {
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        if (wideLen <= 1) return;
        std::wstring widePath((size_t)wideLen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.data(), wideLen);
        HANDLE dir = CreateFileW(widePath.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (dir == INVALID_HANDLE_VALUE) return;
        char buffer[4096];
        DWORD bytes = 0;
        while (sidebarWatchRunning_) {
            if (ReadDirectoryChangesW(dir, buffer, sizeof(buffer), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, &bytes, nullptr, nullptr))
                sidebarRefreshPending_ = true;
        }
        CloseHandle(dir);
    }).detach();
#endif
}

void Application::buildSidebarNode(SidebarNode& node) {
    node.children.clear();
    std::vector<SidebarNode> folders, files;
    try {
        for (auto& entry : fs::directory_iterator(node.path)) {
            SidebarNode child;
            child.path = entry.path().string();
            child.name = entry.path().filename().string();
            child.folder = entry.is_directory();
            child.depth = node.depth + 1;
            child.extension = entry.path().extension().string();
            (child.folder ? folders : files).push_back(std::move(child));
        }
    } catch (...) { return; }
    auto byName = [](const SidebarNode& a, const SidebarNode& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    node.children.reserve(folders.size() + files.size());
    for (auto& child : folders) node.children.push_back(std::move(child));
    for (auto& child : files) node.children.push_back(std::move(child));
}

static void addSolidRect(std::vector<float>& v, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    v.insert(v.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
}

void Application::drawSidebar(FontAtlas& font, float windowH, float titlebarH, float statusbarH) {
    if (!sidebarVisible_) return;
    std::vector<float> v;
    addSolidRect(v, 0, titlebarH, sidebarWidth_, windowH - statusbarH, 0.129f, 0.145f, 0.169f, 1.f);
    addSolidRect(v, sidebarWidth_ - 1.f, titlebarH, sidebarWidth_, windowH - statusbarH, 0.08f, 0.09f, 0.11f, 1.f);
    struct SidebarTextDraw { std::string text; float x, y, r, g, b; };
    std::vector<SidebarTextDraw> textDraws;
    auto fitSidebarText = [&](const std::string& text, float x) {
        float maxW = sidebarWidth_ - x - 8.f;
        if (maxW <= 0.f) return std::string();
        if (font.measureText(text) <= maxW) return text;
        std::string out = text;
        const std::string suffix = "...";
        float suffixW = font.measureText(suffix);
        while (!out.empty() && font.measureText(out) + suffixW > maxW) out.pop_back();
        return out.empty() ? suffix : out + suffix;
    };
    float rowH = 22.f, y = titlebarH + 6.f - sidebarScrollY_;
    sidebarContentH_ = 0.f;
    std::function<void(SidebarNode&)> countNode = [&](SidebarNode& node) {
        if (&node != &sidebarTree_) sidebarContentH_ += rowH;
        if (node.folder && node.expanded) for (auto& child : node.children) countNode(child);
    };
    countNode(sidebarTree_);
    float viewH = windowH - statusbarH - titlebarH - 6.f;
    float maxScroll = std::max(0.f, sidebarContentH_ - viewH);
    if (sidebarScrollY_ > maxScroll) sidebarScrollY_ = maxScroll;
    std::function<void(SidebarNode&)> drawNode = [&](SidebarNode& node) {
        if (&node != &sidebarTree_) {
            float rowY = y; y += rowH;
            if (rowY + rowH < titlebarH || rowY > windowH - statusbarH) return;
            float x = 8.f + node.depth * 14.f;
            bool active = !node.folder && !openFilePath_.empty() && fs::absolute(node.path).string() == fs::absolute(openFilePath_).string();
            if (active) addSolidRect(v, 2.f, rowY - 3.f, sidebarWidth_ - 4.f, rowY + rowH - 3.f, 0.25f, 0.30f, 0.45f, 1.f);
            float ir = node.folder ? 0.86f : 0.45f, ig = node.folder ? 0.66f : 0.55f, ib = node.folder ? 0.22f : 0.65f;
            std::string ext = node.extension;
            if (ext == ".h") { ir = 0.2f; ig = 0.75f; ib = 0.75f; } else if (ext == ".py") { ir = 0.9f; ig = 0.78f; ib = 0.25f; }
            else if (ext == ".js") { ir = 0.9f; ig = 0.55f; ib = 0.2f; } else if (ext == ".json") { ir = 0.45f; ig = 0.75f; ib = 0.35f; }
            else if (ext == ".md") { ir = ig = ib = 0.85f; } else if (ext == ".txt") { ir = ig = ib = 0.55f; }
            addSolidRect(v, x + 12.f, rowY + 3.f, x + 20.f, rowY + 13.f, ir, ig, ib, 1.f);
            if (node.folder) textDraws.push_back({node.expanded ? "v" : ">", x, rowY, 0.55f, 0.55f, 0.58f});
            float b = active ? 0.95f : 0.68f;
            float labelX = x + 26.f;
            textDraws.push_back({fitSidebarText(node.name, labelX), labelX, rowY, b, b, b + 0.03f});
        }
        if (node.folder && node.expanded) for (auto& child : node.children) drawNode(child);
    };
    drawNode(sidebarTree_);
    if (sidebarContentH_ > viewH) {
        float thumbH = std::max(20.f, viewH * (viewH / sidebarContentH_));
        float thumbY = titlebarH + (sidebarScrollY_ / maxScroll) * (viewH - thumbH);
        addSolidRect(v, sidebarWidth_ - 5.f, thumbY, sidebarWidth_ - 1.f, thumbY + thumbH, 0.29f, 0.29f, 0.29f, 1.f);
    }
    if (!v.empty()) {
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, v.size()*sizeof(float), v.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
    }
    for (const auto& td : textDraws) font.drawText(td.text, td.x, td.y, td.r, td.g, td.b, 1.f);
}

bool Application::handleSidebarEvent(const SDL_Event& e, float windowH, float titlebarH, float statusbarH) {
    if (!sidebarVisible_) return false;
    float viewH = windowH - statusbarH - titlebarH - 6.f;
    float maxScroll = std::max(0.f, sidebarContentH_ - viewH);
    auto clampSidebarScroll = [&] {
        if (sidebarScrollY_ < 0.f) sidebarScrollY_ = 0.f;
        if (sidebarScrollY_ > maxScroll) sidebarScrollY_ = maxScroll;
    };
    auto sidebarThumb = [&] (float& thumbY, float& thumbH) {
        thumbH = sidebarContentH_ > 0.f ? std::max(20.f, viewH * (viewH / sidebarContentH_)) : viewH;
        if (thumbH > viewH) thumbH = viewH;
        thumbY = maxScroll > 0.f ? titlebarH + (sidebarScrollY_ / maxScroll) * (viewH - thumbH) : titlebarH;
    };
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 1 && sidebarScrollbarDragging_) {
        sidebarScrollbarDragging_ = false;
        return true;
    }
    if (e.type == SDL_MOUSEMOTION && sidebarScrollbarDragging_) {
        float delta = (float)e.motion.y - sidebarScrollbarDragStartY_;
        float scale = viewH > 0.f ? sidebarContentH_ / viewH : 1.f;
        sidebarScrollY_ = sidebarScrollbarDragStartScrollY_ + delta * scale;
        clampSidebarScroll();
        return true;
    }
    if (e.type == SDL_MOUSEWHEEL && mouseX_ >= 0 && mouseX_ <= sidebarWidth_ && mouseY_ >= titlebarH && mouseY_ <= windowH - statusbarH) {
        sidebarScrollY_ -= e.wheel.y * 44.f;
        clampSidebarScroll();
        return true;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1 && sidebarContentH_ > viewH && e.button.x >= sidebarWidth_ - 8 && e.button.x <= sidebarWidth_ && e.button.y >= titlebarH && e.button.y <= windowH - statusbarH) {
        float thumbY = 0.f, thumbH = viewH;
        sidebarThumb(thumbY, thumbH);
        float my = (float)e.button.y;
        if (my >= thumbY && my <= thumbY + thumbH) {
            sidebarScrollbarDragging_ = true;
            sidebarScrollbarDragStartY_ = my;
            sidebarScrollbarDragStartScrollY_ = sidebarScrollY_;
            return true;
        }
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1 && e.button.x >= sidebarWidth_ - 4 && e.button.x <= sidebarWidth_ + 4 && e.button.y >= titlebarH && e.button.y <= windowH - statusbarH) {
        sidebarResizing_ = true; return true;
    }
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 1) { sidebarResizing_ = false; return false; }
    if (e.type == SDL_MOUSEMOTION && sidebarResizing_) {
        sidebarWidth_ = std::clamp((float)e.motion.x, 140.f, 420.f); return true;
    }
    if (e.type != SDL_MOUSEBUTTONDOWN || e.button.button != 1 || e.button.x > sidebarWidth_ || e.button.y < titlebarH || e.button.y > windowH - statusbarH) return false;
    float rowH = 22.f, y = titlebarH + 6.f - sidebarScrollY_, mx = (float)e.button.x, my = (float)e.button.y;
    std::function<bool(SidebarNode&)> hitNode = [&](SidebarNode& node) -> bool {
        if (&node != &sidebarTree_) {
            float rowY = y; y += rowH;
            if (my >= rowY - 3.f && my < rowY + rowH - 3.f) {
                if (node.folder) {
                    node.expanded = !node.expanded;
                    if (node.expanded && node.children.empty()) buildSidebarNode(node);
                } else openFile(node.path);
                return true;
            }
        }
        if (node.folder && node.expanded) for (auto& child : node.children) if (hitNode(child)) return true;
        return false;
    };
    return hitNode(sidebarTree_);
}

// ── undo/redo ──

void Application::pushUndo() {
    auto now = std::chrono::steady_clock::now();
    if (!undoStack_.empty() && (now - undoStack_.back().time) < undoWindow_) { undoStack_.back().text = textBuffer; undoStack_.back().cursorPos = selections_[0].cursor; return; }
    undoStack_.push_back({textBuffer, selections_[0].cursor, now});
    if (undoStack_.size() > 10000) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}
void Application::doUndo() { if (undoStack_.empty()) return; redoStack_.push_back({textBuffer, selections_[0].cursor, std::chrono::steady_clock::now()}); textBuffer = undoStack_.back().text; selections_.clear(); selections_.emplace_back(undoStack_.back().cursorPos); undoStack_.pop_back(); dirty_ = true; }
void Application::doRedo() { if (redoStack_.empty()) return; undoStack_.push_back({textBuffer, selections_[0].cursor, std::chrono::steady_clock::now()}); textBuffer = redoStack_.back().text; selections_.clear(); selections_.emplace_back(redoStack_.back().cursorPos); redoStack_.pop_back(); dirty_ = true; }

// ── folding ──

bool Application::isFolded(size_t line) const {
    int ln = static_cast<int>(line);
    for (const auto& region : foldedRegions_)
        if (ln > region.first && ln <= region.second) return true;
    return false;
}

bool Application::isFoldStart(size_t line) const {
    int ln = static_cast<int>(line);
    return std::any_of(foldedRegions_.begin(), foldedRegions_.end(), [ln](const auto& region) {
        return region.first == ln;
    });
}

bool Application::isFoldableLine(size_t line) const {
    if (line >= totalLines()) return false;
    size_t ls = lineStartForLine(line), le = lineEnd(ls);
    while (le > ls && (textBuffer[le - 1] == ' ' || textBuffer[le - 1] == '\t')) --le;
    if (le > ls && textBuffer[le - 1] == '{') return true;
    if (line + 1 < totalLines()) {
        int curInd = (line < lineIndents_.size()) ? lineIndents_[line] : 0;
        int nxtInd = (line + 1 < lineIndents_.size()) ? lineIndents_[line + 1] : 0;
        return nxtInd > curInd;
    }
    return false;
}

size_t Application::findFoldEnd(size_t startLine) const {
    size_t ls = lineStartForLine(startLine), le = lineEnd(ls);
    size_t trimmed = le;
    while (trimmed > ls && (textBuffer[trimmed - 1] == ' ' || textBuffer[trimmed - 1] == '\t')) --trimmed;
    if (trimmed > ls && textBuffer[trimmed - 1] == '{') {
        int depth = 1;
        for (size_t pos = trimmed; pos < textBuffer.size(); ++pos) {
            if (textBuffer[pos] == '{') ++depth;
            else if (textBuffer[pos] == '}') {
                if (--depth == 0) return lineOfPos(pos);
            }
        }
    }
    if (startLine + 1 < totalLines()) {
        int baseInd = (startLine < lineIndents_.size()) ? lineIndents_[startLine] : 0;
        size_t end = startLine;
        for (size_t ln = startLine + 1; ln < totalLines(); ++ln) {
            int ind = (ln < lineIndents_.size()) ? lineIndents_[ln] : 0;
            size_t rowStart = lineStartForLine(ln), rowEnd = lineEnd(rowStart);
            bool blank = rowStart == rowEnd;
            if (!blank && ind <= baseInd) break;
            end = ln;
        }
        return end;
    }
    return startLine;
}

void Application::toggleFold(size_t line) {
    int ln = static_cast<int>(line);
    auto it = std::find_if(foldedRegions_.begin(), foldedRegions_.end(), [ln](const auto& region) {
        return region.first == ln;
    });
    if (it != foldedRegions_.end()) { foldedRegions_.erase(it); return; }
    computeLineIndents();
    if (!isFoldableLine(line)) return;
    size_t end = findFoldEnd(line);
    if (end > line) foldedRegions_.insert({ln, static_cast<int>(end)});
}

void Application::foldAll() {
    computeLineIndents();
    foldedRegions_.clear();
    for (size_t ln = 0; ln < totalLines(); ++ln) {
        if (!isFoldableLine(ln)) continue;
        size_t end = findFoldEnd(ln);
        if (end > ln) foldedRegions_.insert({static_cast<int>(ln), static_cast<int>(end)});
    }
}

void Application::unfoldAll() {
    foldedRegions_.clear();
}

void Application::ensurePopupWindow() {
    if (popupWin_) return;
    popupWin_ = SDL_CreateWindow("popup", 0, 0, 1, 1,
        SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
#ifdef _WIN32
    SDL_SysWMinfo wmInfo; SDL_VERSION(&wmInfo.version);
    if (popupWin_ && SDL_GetWindowWMInfo(popupWin_, &wmInfo)) {
        HWND hwnd = reinterpret_cast<HWND>(wmInfo.info.win.window);
        LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
        SetLayeredWindowAttributes(hwnd, RGB(30, 30, 30), 0, LWA_COLORKEY);
    }
#endif
}

void Application::hidePopupWindow() {
    if (popupWin_) SDL_HideWindow(popupWin_);
}

void Application::shapePopupWindowForMenu() {
#ifdef _WIN32
    if (!popupWin_) return;
    SDL_SysWMinfo wmInfo; SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(popupWin_, &wmInfo)) return;
    SDL_Rect mainRect{}, submenuRect{}; bool hasSubmenu = false;
    titlebar_->getMenuPanelRects(fontAtlas(), 32000, 32000, mainRect, submenuRect, hasSubmenu);
    HRGN region = CreateRectRgn(mainRect.x + 3, mainRect.y + 3, mainRect.x + mainRect.w + 3, mainRect.y + mainRect.h + 3);
    if (hasSubmenu && submenuRect.w > 0 && submenuRect.h > 0) {
        HRGN submenu = CreateRectRgn(submenuRect.x + 3, submenuRect.y + 3, submenuRect.x + submenuRect.w + 3, submenuRect.y + submenuRect.h + 3);
        CombineRgn(region, region, submenu, RGN_OR);
        DeleteObject(submenu);
    }
    SetWindowRgn(reinterpret_cast<HWND>(wmInfo.info.win.window), region, TRUE);
#endif
}

void Application::clearPopupWindowShape() {
#ifdef _WIN32
    if (!popupWin_) return;
    SDL_SysWMinfo wmInfo; SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(popupWin_, &wmInfo)) SetWindowRgn(reinterpret_cast<HWND>(wmInfo.info.win.window), nullptr, TRUE);
#endif
}

static void drawPopupClearRect(int w, int h) {
    std::vector<float> verts = {0,0,0,0,0.118f,0.118f,0.118f,1.f, 0,(float)h,0,0,0.118f,0.118f,0.118f,1.f, (float)w,(float)h,0,0,0.118f,0.118f,0.118f,1.f,
        0,0,0,0,0.118f,0.118f,0.118f,1.f, (float)w,(float)h,0,0,0.118f,0.118f,0.118f,1.f, (float)w,0,0,0,0.118f,0.118f,0.118f,1.f};
    GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0); GLRenderer::setDrawMode(0);
}

void Application::renderPopupToWindow(int x, int y, int w, int h) {
    ensurePopupWindow();
    popupScreenX_ = x; popupScreenY_ = y;
    SDL_SetWindowPosition(popupWin_, x, y);
    SDL_SetWindowSize(popupWin_, w, h);
    SDL_ShowWindow(popupWin_);
    if (SDL_GL_MakeCurrent(popupWin_, glContext_) != 0) return;
    int pw, ph; SDL_GL_GetDrawableSize(popupWin_, &pw, &ph);
    GLRenderer::resize(pw, ph);
    GLRenderer::beginFrame();
    drawPopupClearRect(pw, ph);
}

void Application::convertIndentation(bool toSpaces) {
    pushUndo();
    std::string result; result.reserve(textBuffer.size());
    size_t i = 0;
    while (i < textBuffer.size()) {
        if (i == 0 || textBuffer[i-1] == '\n') {
            int spaces = 0;
            while (i < textBuffer.size() && textBuffer[i] == ' ') { ++spaces; ++i; }
            int tabs = 0;
            while (i < textBuffer.size() && textBuffer[i] == '\t') { ++tabs; ++i; }
            if (toSpaces) {
                int totalSpaces = spaces + tabs * tabSize_;
                result.append(totalSpaces, ' ');
            } else {
                int totalCols = spaces + tabs * tabSize_;
                int fullTabs = totalCols / tabSize_;
                int rem = totalCols % tabSize_;
                result.append(fullTabs, '\t');
                result.append(rem, ' ');
            }
        } else {
            result += textBuffer[i]; ++i;
        }
    }
    textBuffer = std::move(result);
    dirty_ = true;
}

void Application::guessIndent() {
    size_t i = 0, spaceLines = 0, tabLines = 0, totalSpaceWidth = 0, spaceWidthCount = 0;
    while (i < textBuffer.size()) {
        if (i == 0 || textBuffer[i-1] == '\n') {
            if (i < textBuffer.size() && textBuffer[i] == '\t') { ++tabLines; ++i; }
            else {
                int sp = 0;
                while (i < textBuffer.size() && textBuffer[i] == ' ') { ++sp; ++i; }
                if (sp > 0) { ++spaceLines; totalSpaceWidth += sp; ++spaceWidthCount; }
            }
        } else ++i;
    }
    if (spaceLines == 0 && tabLines == 0) return;
    useTabs_ = (tabLines > spaceLines);
    if (spaceWidthCount > 0) {
        int avg = (int)(totalSpaceWidth / spaceWidthCount);
        // snap to nearest common indent size
        int best = 2;
        for (int c : {2,3,4,8}) { if (abs(c - avg) < abs(best - avg)) best = c; }
        tabSize_ = best;
    }
    syntax_->setTabSize(tabSize_);
    syntax_->setUseTabs(useTabs_);
}

void Application::computeLineIndents() {
    if (!indentsDirty_) return;
    indentsDirty_ = false;
    lineIndents_.resize(totalLines(), 0);
    size_t li = 0, pos = 0;
    while (pos <= textBuffer.size() && li < lineIndents_.size()) {
        int indent = 0;
        while (pos + indent < textBuffer.size() && textBuffer[pos + indent] == ' ') ++indent;
        int extra = indent;
        while (pos + extra < textBuffer.size() && textBuffer[pos + extra] == '\t') ++extra;
        lineIndents_[li] = (extra > indent) ? (indent + (extra - indent) * tabSize_) : indent;
        ++li;
        size_t nl = textBuffer.find('\n', pos);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

void Application::computeMaxLineWidth() {
    if (!maxLineWidthDirty_) return;
    maxLineWidthDirty_ = false;
    maxLineWidth_ = 0.f;
    size_t pos = 0;
    while (pos <= textBuffer.size()) {
        size_t end = textBuffer.find('\n', pos);
        if (end == std::string::npos) end = textBuffer.size();
        maxLineWidth_ = std::max(maxLineWidth_, fontAtlas().measureText(std::string_view(textBuffer.data() + pos, end - pos)));
        if (end >= textBuffer.size()) break;
        pos = end + 1;
    }
}

// ── tabs ──

void Application::saveCurrentTab() {
    if (activeTab_ >= tabs_.size()) return;
    auto& tab = tabs_[activeTab_];
    tab.text = textBuffer; tab.filePath = openFilePath_; tab.fileName = openFile_;
    tab.selections = selections_; tab.scrollY = scrollY_;
    tab.undoStack = std::move(undoStack_); tab.redoStack = std::move(redoStack_);
    tab.foldedRegions = foldedRegions_; tab.dirty = dirty_; tab.desiredCursorX = desiredCursorX_;
}

void Application::loadTab(size_t index) {
    if (index >= tabs_.size()) return;
    auto& tab = tabs_[index];
    textBuffer = tab.text; openFilePath_ = tab.filePath; openFile_ = tab.fileName;
    selections_ = tab.selections; scrollY_ = tab.scrollY;
    undoStack_ = std::move(tab.undoStack); redoStack_ = std::move(tab.redoStack);
    foldedRegions_ = tab.foldedRegions; dirty_ = tab.dirty; desiredCursorX_ = tab.desiredCursorX;
    activeTab_ = index; detectSyntax(); updateGitBranch();
}

void Application::switchToTab(size_t index) {
    if (index >= tabs_.size() || index == activeTab_) return;
    saveCurrentTab(); loadTab(index);
}

void Application::closeTabNow(size_t index) {
    if (index >= tabs_.size()) return;
    if (!tabs_[index].filePath.empty()) {
        closedTabStack_.push_back(tabs_[index].filePath);
        if (closedTabStack_.size() > 10) closedTabStack_.erase(closedTabStack_.begin());
    }
    tabs_.erase(tabs_.begin() + index);
    if (tabs_.empty()) { newBuffer(); return; }
    if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.size()-1;
    loadTab(activeTab_);
}

void Application::closeTab(size_t index) {
    saveCurrentTab();
    if (index >= tabs_.size()) return;
    if (tabs_[index].dirty) { closeConfirmOpen_ = true; closeConfirmIndex_ = index; return; }
    closeTabNow(index);
}

void Application::reopenClosedTab() {
    while (!closedTabStack_.empty()) {
        std::string path = closedTabStack_.back();
        closedTabStack_.pop_back();
        if (fs::exists(path)) { openFile(path); return; }
    }
}

void Application::drawTabBar(FontAtlas& font, float windowW, float titlebarH) {
    auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        solidVerts_.insert(solidVerts_.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    float barY = titlebarH;
    float controlsW = 60.f;
    float visibleW = windowW - controlsW;
    ar(0, barY, windowW, barY + tabBarH_, 0.25f, 0.25f, 0.28f, 1.f);
    float totalTabsW = 0.f;
    for (auto& tab : tabs_) {
        std::string label = tab.fileName.empty() ? "untitled" : tab.fileName;
        if (tab.dirty) label += "\xe2\x80\xa2";
        totalTabsW += std::clamp(font.measureText(label) + 32.f, 48.f, 180.f);
    }
    float maxTabScroll = totalTabsW > visibleW ? totalTabsW - visibleW : 0.f;
    if (tabScrollX_ > maxTabScroll) tabScrollX_ = maxTabScroll;
    if (tabScrollX_ < 0.f) tabScrollX_ = 0.f;
    float tx = -tabScrollX_;
    struct TabDrawLabel { size_t index; float x; std::string text; };
    std::vector<TabDrawLabel> labels;
    labels.reserve(tabs_.size());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
        if (tabs_[i].dirty) label += "\xe2\x80\xa2";
        float tw = std::clamp(font.measureText(label) + 32.f, 48.f, 180.f);
        if (tx + tw < 0.f) { tx += tw; continue; }
        if (tx > visibleW) break;
        if (i == activeTab_) {
            ar(tx, barY, tx + tw, barY + tabBarH_, 0.20f, 0.20f, 0.23f, 1.f);
            ar(tx, barY + tabBarH_ - 2.f, tx + tw, barY + tabBarH_, accentColor_.r, accentColor_.g, accentColor_.b, 0.8f);
        }
        labels.push_back({i, tx, label});
        tx += tw;
    }
    ar(visibleW, barY, windowW, barY + tabBarH_, 0.18f, 0.18f, 0.21f, 1.f);
    for (int i = 0; i < 3; ++i) {
        float x = visibleW + i * 20.f;
        ar(x, barY, x + 20.f, barY + tabBarH_, 0.21f, 0.21f, 0.24f, 1.f);
    }
    auto flushTabSolids = [&] {
        if (solidVerts_.empty()) return;
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, solidVerts_.size()*sizeof(float), solidVerts_.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(solidVerts_.size()/8));
        glBindVertexArray(0); GLRenderer::setDrawMode(0); solidVerts_.clear();
    };
    flushTabSolids();
    for (auto& label : labels) {
        float lx = label.x;
        float textBright = (label.index == activeTab_) ? 0.85f : 0.55f;
        float textY = barY + 8.f;
        font.drawText(label.text, lx + 12.f, textY, textBright, textBright, textBright+0.05f, 1.f);
        float tw = std::clamp(font.measureText(label.text) + 32.f, 48.f, 180.f);
        font.drawText("\xc3\x97", lx + tw - 17.f, textY, 0.55f, 0.55f, 0.60f, 1.f);
    }
    float activeBright = maxTabScroll > 0.f ? 0.8f : 0.35f;
    float controlY = barY + 8.f;
    tabChevronX_ = visibleW + 40.f;
    font.drawText("<", visibleW + 7.f, controlY, tabScrollX_ > 0.f ? activeBright : 0.35f, tabScrollX_ > 0.f ? activeBright : 0.35f, tabScrollX_ > 0.f ? activeBright : 0.35f, 1.f);
    font.drawText(">", visibleW + 27.f, controlY, tabScrollX_ < maxTabScroll ? activeBright : 0.35f, tabScrollX_ < maxTabScroll ? activeBright : 0.35f, tabScrollX_ < maxTabScroll ? activeBright : 0.35f, 1.f);
    font.drawText("v", visibleW + 47.f, controlY, 0.75f, 0.75f, 0.78f, 1.f);
    // tab drag ghost
    if (tabDragging_ && tabDragIndex_ < tabs_.size()) {
        std::string dragLabel = tabs_[tabDragIndex_].fileName.empty() ? "untitled" : tabs_[tabDragIndex_].fileName;
        if (tabs_[tabDragIndex_].dirty) dragLabel += "\xe2\x80\xa2";
        float dragW = std::clamp(font.measureText(dragLabel) + 32.f, 48.f, 180.f);
        float dragX = std::clamp(mouseX_ - dragW / 2.f, 0.f, visibleW - dragW);
        ar(dragX, barY, dragX + dragW, barY + tabBarH_, 0.20f, 0.20f, 0.23f, 0.6f);
        ar(dragX, barY + tabBarH_ - 2.f, dragX + dragW, barY + tabBarH_, accentColor_.r, accentColor_.g, accentColor_.b, 0.5f);
        flushTabSolids();
        font.drawText(dragLabel, dragX + 12.f, barY + 8.f, 0.85f, 0.85f, 0.85f, 0.6f);
    }
    if (tabDropdownOpen_ && !deferPopupDraw_) {
        float popW = 260.f, itemH = 24.f, popH = tabs_.size() * itemH + 4.f, popX = tabChevronX_, popY = barY + tabBarH_;;
        std::vector<float> pv;
        auto pr = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            pv.insert(pv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        pr(popX, popY, popX + popW, popY + popH, 0.17f, 0.17f, 0.20f, 0.98f);
        if (tabDropdownHover_ >= 0 && tabDropdownHover_ < (int)tabs_.size())
            pr(popX + 2, popY + 2 + tabDropdownHover_ * itemH, popX + popW - 2, popY + 2 + (tabDropdownHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, pv.size()*sizeof(float), pv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(pv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        for (size_t i = 0; i < tabs_.size(); ++i) {
            std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
            if (tabs_[i].dirty) label += "\xe2\x80\xa2";
            float b = i == activeTab_ ? 0.95f : 0.70f;
            font.drawText(label, popX + 10.f, popY + 6.f + i * itemH, b, b, b + 0.03f, 1.f);
        }
    }
    if (tabContextOpen_ && !deferPopupDraw_) {
        const char* items[] = {"Close Tab","Close Other Tabs","Close Tabs to the Right","Close Unmodified Tabs","Close Unmodified Tabs to the Right","Close Tabs With Deleted Files","","Split View","","New File","Open File    Ctrl+O","","Diff With Current Tab..."};
        int itemCount = tabContextIndex_ != activeTab_ ? 13 : 12;
        float itemH = 24.f, popW = 260.f, popH = 4.f + itemCount * itemH;
        std::vector<float> cv;
        auto cr = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            cv.insert(cv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        cr(tabContextX_, tabContextY_, tabContextX_ + popW, tabContextY_ + popH, 0.17f, 0.17f, 0.20f, 0.98f);
        if (tabContextHover_ >= 0 && items[tabContextHover_][0]) cr(tabContextX_ + 2, tabContextY_ + 2 + tabContextHover_ * itemH, tabContextX_ + popW - 2, tabContextY_ + 2 + (tabContextHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, cv.size()*sizeof(float), cv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(cv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        for (int i = 0; i < itemCount; ++i) {
            if (!items[i][0]) { std::vector<float> sv; cr(tabContextX_ + 8.f, tabContextY_ + 13.f + i * itemH, tabContextX_ + popW - 8.f, tabContextY_ + 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f); continue; }
            font.drawText(items[i], tabContextX_ + 10.f, tabContextY_ + 6.f + i * itemH, 0.78f, 0.78f, 0.82f, 1.f);
        }
    }
}

bool Application::handleTabBarEvent(const SDL_Event& e, float windowW, float titlebarH) {
    float barY = titlebarH;
    float controlsW = 60.f, visibleW = windowW - controlsW;
    float totalTabsW = 0.f;
    for (auto& tab : tabs_) {
        std::string label = tab.fileName.empty() ? "untitled" : tab.fileName;
        if (tab.dirty) label += "\xe2\x80\xa2";
        totalTabsW += std::clamp(fontAtlas().measureText(label) + 32.f, 48.f, 180.f);
    }
    float maxTabScroll = totalTabsW > visibleW ? totalTabsW - visibleW : 0.f;
    if (e.type == SDL_MOUSEMOTION && tabDragging_) {
        float dx = (float)e.motion.x - tabDragStartX_;
        if (std::abs(dx) > 4.f) {
            float dropX = (float)e.motion.x + tabScrollX_;
            float tx = 0.f;
            size_t newIdx = tabs_.size();
            for (size_t i = 0; i < tabs_.size(); ++i) {
                std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
                if (tabs_[i].dirty) label += "\xe2\x80\xa2";
                float tw = std::clamp(fontAtlas().measureText(label) + 32.f, 48.f, 180.f);
                if (dropX < tx + tw / 2.f) { newIdx = i; break; }
                tx += tw;
            }
            if (newIdx != tabDragIndex_ && newIdx < tabs_.size()) {
                auto tab = std::move(tabs_[tabDragIndex_]);
                tabs_.erase(tabs_.begin() + tabDragIndex_);
                if (newIdx > tabDragIndex_) --newIdx;
                tabs_.insert(tabs_.begin() + newIdx, std::move(tab));
                activeTab_ = newIdx;
                tabDragIndex_ = newIdx;
                tabDragStartX_ = (float)e.motion.x;
            }
        }
        return true;
    }
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 1 && tabDragging_) { tabDragging_ = false; return true; }
    if (e.type == SDL_MOUSEMOTION && tabContextOpen_) {
        float mx = (float)e.motion.x, my = (float)e.motion.y, itemH = 24.f, popW = 260.f;
        int itemCount = tabContextIndex_ != activeTab_ ? 13 : 12;
        float popH = 4.f + itemCount * itemH;
        tabContextHover_ = (mx >= tabContextX_ && mx <= tabContextX_ + popW && my >= tabContextY_ && my <= tabContextY_ + popH) ? (int)((my - tabContextY_ - 2.f) / itemH) : -1;
        if (tabContextHover_ == 6 || tabContextHover_ == 8 || tabContextHover_ == 11) tabContextHover_ = -1;
        return true;
    }
    if (e.type == SDL_MOUSEMOTION && tabDropdownOpen_) {
        float mx = (float)e.motion.x, my = (float)e.motion.y;
        float popW = 260.f, itemH = 24.f, popX = tabChevronX_, popY = barY + tabBarH_;
        tabDropdownHover_ = (mx >= popX && mx <= popX + popW && my >= popY && my <= popY + tabs_.size() * itemH + 4.f) ? (int)((my - popY - 2.f) / itemH) : -1;
        return true;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && (e.button.button == 1 || e.button.button == 2 || e.button.button == 3)) {
        float mx = (float)e.button.x, my = (float)e.button.y;
        if (tabContextOpen_) {
            float itemH = 24.f, popW = 260.f;
            int itemCount = tabContextIndex_ != activeTab_ ? 13 : 12;
            float popH = 4.f + itemCount * itemH;
            if (mx >= tabContextX_ && mx <= tabContextX_ + popW && my >= tabContextY_ && my <= tabContextY_ + popH) {
                int idx = (int)((my - tabContextY_ - 2.f) / itemH);
                if (idx == 0) closeTab(tabContextIndex_);
                else if (idx == 1) { for (size_t i = tabs_.size(); i-- > 0;) if (i != tabContextIndex_) { if (tabs_[i].dirty) closeTab(i); else closeTabNow(i); if (closeConfirmOpen_) break; } }
                else if (idx == 2) { for (size_t i = tabs_.size(); i-- > tabContextIndex_ + 1;) { if (tabs_[i].dirty) closeTab(i); else closeTabNow(i); if (closeConfirmOpen_) break; } }
                else if (idx == 3) { for (size_t i = tabs_.size(); i-- > 0;) if (!tabs_[i].dirty) closeTabNow(i); }
                else if (idx == 4) { for (size_t i = tabs_.size(); i-- > tabContextIndex_ + 1;) if (!tabs_[i].dirty) closeTabNow(i); }
                else if (idx == 5) { for (size_t i = tabs_.size(); i-- > 0;) if (!tabs_[i].filePath.empty() && !fs::exists(tabs_[i].filePath)) closeTabNow(i); }
                else if (idx == 9) newBuffer();
                else if (idx == 10) openFileDialog();
                tabContextOpen_ = false; return true;
            }
            tabContextOpen_ = false;
            return true;
        }
        if (tabDropdownOpen_) {
            float popW = 260.f, itemH = 24.f, popX = tabChevronX_, popY = barY + tabBarH_;
            if (mx >= popX && mx <= popX + popW && my >= popY && my <= popY + tabs_.size() * itemH + 4.f) {
                int idx = (int)((my - popY - 2.f) / itemH);
                if (idx >= 0 && idx < (int)tabs_.size()) switchToTab((size_t)idx);
                tabDropdownOpen_ = false; return true;
            }
            tabDropdownOpen_ = false;
            return true;
        }
        if (my < barY || my >= barY + tabBarH_) return false;
        if (mx >= visibleW) {
            int btn = (int)((mx - visibleW) / 20.f);
            if (btn == 0 && maxTabScroll > 0.f) tabScrollX_ -= 80.f;
            else if (btn == 1 && maxTabScroll > 0.f) tabScrollX_ += 80.f;
            else if (btn == 2) { tabDropdownOpen_ = !tabDropdownOpen_; tabDropdownHover_ = -1; }
            if (tabScrollX_ < 0.f) tabScrollX_ = 0.f;
            if (tabScrollX_ > maxTabScroll) tabScrollX_ = maxTabScroll;
            return true;
        }
        float tx = -tabScrollX_;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
            if (tabs_[i].dirty) label += "\xe2\x80\xa2";
            float tw = std::clamp(fontAtlas().measureText(label) + 32.f, 48.f, 180.f);
            if (mx >= tx && mx < tx + tw) {
                if (e.button.button == 3) { tabContextOpen_ = true; tabContextIndex_ = i; tabContextX_ = mx; tabContextY_ = barY + tabBarH_ + 2.f; tabContextHover_ = -1; return true; }
                if (e.button.button == 2 || mx >= tx + tw - 20.f) { closeTab(i); return true; }
                switchToTab(i);
                if (e.button.button == 1 && mx < tx + tw - 20.f) { tabDragging_ = true; tabDragIndex_ = i; tabDragStartX_ = mx; }
                return true;
            }
            tx += tw;
        }
        return false;
    }
    return false;
}

// ── static data ──

const char* Application::syntaxLanguages[] = {
    "Plain Text","ActionScript","AppleScript","ASP","Batch File","C","C#","C++","Clojure","CSS",
    "D","Diff","Erlang","Go","Graphviz","Groovy","Haskell","HTML","Java","JavaScript",
    "JSON","LaTeX","Lisp","Lua","Makefile","Markdown","MATLAB","Objective-C","OCaml","Pascal",
    "Perl","PHP","Python","R","Ruby","Rust","Scala","ShellScript","SQL","MorenoRC",
    "TCL","TOML","XML","YAML"
};

struct PaletteCommandDef {
    const char* label;
    const char* shortcut;
};

static const PaletteCommandDef paletteCommands[] = {
    {"New File","Ctrl+N"},{"Open File","Ctrl+O"},{"Open Folder",""},{"Save","Ctrl+S"},{"Save As","Ctrl+Shift+S"},{"Save All",""},
    {"Close File","Ctrl+W"},{"Reopen Closed Tab","Ctrl+Shift+T"},{"Revert File",""},{"Undo","Ctrl+Z"},{"Redo","Ctrl+Y"},
    {"Cut","Ctrl+X"},{"Copy","Ctrl+C"},{"Paste","Ctrl+V"},{"Paste and Indent","Ctrl+Shift+V"},{"Select All","Ctrl+A"},
    {"Find","Ctrl+F"},{"Replace","Ctrl+H"},{"Goto Anything","Ctrl+P"},{"Toggle Side Bar","Ctrl+K Ctrl+B"},{"Toggle Minimap",""},
    {"Enter Full Screen","F11"},{"Toggle Word Wrap",""},
    {"Swap Line Up","Ctrl+Shift+Up"},{"Swap Line Down","Ctrl+Shift+Down"},{"Duplicate Line","Ctrl+Shift+D"},{"Delete Line","Ctrl+Shift+K"},{"Join Lines","Ctrl+Shift+J"},
    {"Toggle Line Comment","Ctrl+/"},{"Toggle Block Comment","Ctrl+Shift+/"},
    {"Upper Case","Ctrl+K Ctrl+U"},{"Lower Case","Ctrl+K Ctrl+L"},{"Title Case",""},{"Swap Case",""},
    {"Toggle Bookmark","Ctrl+F2"},{"Next Bookmark","F2"},{"Previous Bookmark","Shift+F2"},{"Clear All Bookmarks","Ctrl+Shift+F2"}
};

static std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

void Application::updateCommandPalette() {
    commandPalette_.results.clear();
    std::string q = lowerCopy(commandPalette_.query);
    int count = (int)(sizeof(paletteCommands) / sizeof(paletteCommands[0]));
    for (int i = 0; i < count; ++i) {
        std::string label = lowerCopy(paletteCommands[i].label);
        if (q.empty() || label.find(q) != std::string::npos) commandPalette_.results.push_back(i);
    }
    if (commandPalette_.selected >= (int)commandPalette_.results.size()) commandPalette_.selected = (int)commandPalette_.results.size() - 1;
    if (commandPalette_.selected < 0) commandPalette_.selected = commandPalette_.results.empty() ? -1 : 0;
    int maxScroll = std::max(0, (int)commandPalette_.results.size() - 12);
    if (commandPalette_.scroll > maxScroll) commandPalette_.scroll = maxScroll;
    if (commandPalette_.selected >= 0 && commandPalette_.selected < commandPalette_.scroll) commandPalette_.scroll = commandPalette_.selected;
    if (commandPalette_.selected >= commandPalette_.scroll + 12) commandPalette_.scroll = commandPalette_.selected - 11;
    if (commandPalette_.scroll < 0) commandPalette_.scroll = 0;
}

void Application::executePaletteCommand(int commandIndex) {
    switch (commandIndex) {
        case 0: newBuffer(); break;
        case 1: openFileDialog(); break;
        case 2: openFolderDialog(); break;
        case 3: saveFile(); break;
        case 4: saveFileAs(); break;
        case 5: saveAll(); break;
        case 6: closeCurrentTab(); break;
        case 7: reopenClosedTab(); break;
        case 8: revertFile(); break;
        case 9: doUndo(); break;
        case 10: doRedo(); break;
        case 11: cutSelectionOrLine(); break;
        case 12: copySelectionOrLine(); break;
        case 13: pasteClipboard(false); break;
        case 14: pasteClipboard(true); break;
        case 15: commandSelectAll(); break;
        case 16: commandFind(); break;
        case 17: commandReplace(); break;
        case 18: commandGotoAnything(); break;
        case 19: toggleSidebar(); break;
        case 20: toggleMinimap(); break;
        case 21: toggleFullscreen(); break;
        case 22: wordWrap_ = !wordWrap_; break;
        case 23: swapLineUp(); break;
        case 24: swapLineDown(); break;
        case 25: duplicateLine(); break;
        case 26: deleteLine(); break;
        case 27: joinLines(); break;
        case 28: toggleLineComment(); break;
        case 29: toggleBlockComment(); break;
        case 30: convertCaseUpper(); break;
        case 31: convertCaseLower(); break;
        case 32: convertCaseTitle(); break;
        case 33: convertCaseSwap(); break;
        case 34: toggleBookmark(); break;
        case 35: nextBookmark(); break;
        case 36: prevBookmark(); break;
        case 37: clearBookmarks(); break;
    }
}

void Application::updateGotoResults() {
    goto_.items.clear(); goto_.subtexts.clear(); goto_.scores.clear();
    const std::string& q = goto_.query;
    if (q.empty()) return;
    if (q[0] == '@') {
        gotoMode_ = Symbols;
        std::string pat = q.substr(1);
        std::regex re(R"((?:function|def|class|struct|void|int|string|var|let|const|async)\s+(\w+))", std::regex::icase);
        for (auto it = std::sregex_iterator(textBuffer.begin(), textBuffer.end(), re); it != std::sregex_iterator(); ++it) {
            std::string name = (*it)[1].str();
            size_t pos = (size_t)it->position() + (*it)[0].str().find(name);
            size_t ln = lineOfPos(pos) + 1;
            if (pat.empty() || name.find(pat) != std::string::npos) {
                goto_.items.push_back(name);
                goto_.subtexts.push_back("line " + std::to_string(ln));
            }
        }
    } else if (q[0] == ':') {
        gotoMode_ = Lines;
        int ln = 0; for (char c : q.substr(1)) if (isdigit(c)) ln = ln * 10 + (c - '0');
        if (ln > 0 && ln <= (int)totalLines()) {
            size_t p = lineStartForLine(ln - 1);
            selections_[0].anchor = selections_[0].cursor = p;
            ensureCursorVisible();
        }
    } else if (q[0] == '#') {
        gotoMode_ = Words;
        std::string pat = q.substr(1);
        if (pat.empty()) return;
        std::unordered_set<std::string> seen;
        std::string word;
        for (size_t i = 0; i < textBuffer.size(); ++i) {
            char c = textBuffer[i];
            if (isalnum(c) || c == '_') word += c;
            else { if (word.size() >= 2 && word.find(pat) != std::string::npos && seen.insert(word).second) goto_.items.push_back(word); word.clear(); }
        }
        if (word.size() >= 2 && word.find(pat) != std::string::npos && seen.insert(word).second) goto_.items.push_back(word);
    } else {
        gotoMode_ = Files;
        for (auto& tab : tabs_) {
            if (tab.fileName.find(q) != std::string::npos) goto_.items.push_back(tab.fileName);
        }
        if (!sidebarRoot_.empty()) {
            try {
                for (auto& entry : fs::recursive_directory_iterator(sidebarRoot_)) {
                    if (entry.is_directory()) continue;
                    std::string name = entry.path().filename().string();
                    if (name.find(q) != std::string::npos) {
                        goto_.items.push_back(name);
                        goto_.subtexts.push_back(fs::relative(entry.path(), sidebarRoot_).string());
                    }
                    if (goto_.items.size() >= 20) break;
                }
            } catch (...) {}
        }
    }
    goto_.selected = goto_.items.empty() ? -1 : 0;
}

void Application::updateAutocomplete() {
    acItems_.clear(); acActive_ = false;
    if (acPrefix_.size() < 2) return;
    std::unordered_set<std::string> seen;
    seen.insert(acPrefix_);
    std::string word;
    for (size_t i = 0; i < textBuffer.size(); ++i) {
        char c = textBuffer[i];
        if (isalnum(c) || c == '_') word += c;
        else {
            if (word.size() >= acPrefix_.size() && word != acPrefix_) {
                bool match = true;
                for (size_t j = 0; j < acPrefix_.size() && match; ++j)
                    if (tolower(word[j]) != tolower(acPrefix_[j])) match = false;
                if (match && seen.insert(word).second) acItems_.push_back(word);
            }
            word.clear();
        }
    }
    if (word.size() >= acPrefix_.size() && word != acPrefix_) {
        bool match = true;
        for (size_t j = 0; j < acPrefix_.size() && match; ++j)
            if (tolower(word[j]) != tolower(acPrefix_[j])) match = false;
        if (match && seen.insert(word).second) acItems_.push_back(word);
    }
    if (!acItems_.empty()) { acActive_ = true; acSelected_ = 0; }
    if (acItems_.size() > 10) acItems_.resize(10);
}

void Application::acceptAutocomplete() {
    if (!acActive_ || acSelected_ >= (int)acItems_.size()) return;
    std::string completion = acItems_[acSelected_].substr(acPrefix_.size());
    size_t pos = selections_[0].cursor;
    textBuffer.insert(pos, completion);
    selections_[0].anchor = selections_[0].cursor = pos + completion.size();
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
    acActive_ = false;
}

// ── line operations ──

void Application::swapLineUp() {
    size_t cl = lineOfPos(selections_[0].cursor);
    if (cl == 0) return;
    pushUndo();
    size_t ls = lineStartForLine(cl), le = lineEnd(ls);
    if (le < textBuffer.size()) ++le;
    size_t prevLs = lineStartForLine(cl - 1);
    std::string cur = textBuffer.substr(ls, le - ls);
    std::string prev = textBuffer.substr(prevLs, ls - prevLs);
    textBuffer.replace(prevLs, le - prevLs, cur + prev);
    selections_[0].anchor = selections_[0].cursor = prevLs + (selections_[0].cursor - ls);
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::swapLineDown() {
    size_t cl = lineOfPos(selections_[0].cursor);
    if (cl >= totalLines() - 1) return;
    pushUndo();
    size_t ls = lineStartForLine(cl), le = lineEnd(ls);
    size_t nextLs = le < textBuffer.size() ? le + 1 : le;
    size_t nextLe = lineEnd(nextLs);
    if (nextLe < textBuffer.size()) ++nextLe;
    std::string cur = textBuffer.substr(ls, nextLs - ls);
    std::string next = textBuffer.substr(nextLs, nextLe - nextLs);
    textBuffer.replace(ls, nextLe - ls, next + cur);
    size_t offset = next.size();
    selections_[0].anchor = selections_[0].cursor = ls + offset + (selections_[0].cursor - ls);
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::duplicateLine() {
    pushUndo();
    size_t cl = lineOfPos(selections_[0].cursor);
    size_t ls = lineStartForLine(cl), le = lineEnd(ls);
    std::string line = textBuffer.substr(ls, le - ls);
    std::string ins = "\n" + line;
    textBuffer.insert(le, ins);
    selections_[0].anchor = selections_[0].cursor = le + 1 + (selections_[0].cursor - ls);
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::deleteLine() {
    pushUndo();
    size_t cl = lineOfPos(selections_[0].cursor);
    size_t ls = lineStartForLine(cl), le = lineEnd(ls);
    if (le < textBuffer.size()) ++le;
    textBuffer.erase(ls, le - ls);
    selections_[0].anchor = selections_[0].cursor = std::min(ls, textBuffer.size());
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::joinLines() {
    size_t cl = lineOfPos(selections_[0].cursor);
    if (cl >= totalLines() - 1) return;
    pushUndo();
    size_t le = lineEnd(selections_[0].cursor);
    if (le >= textBuffer.size()) return;
    textBuffer.erase(le, 1);
    while (le < textBuffer.size() && (textBuffer[le] == ' ' || textBuffer[le] == '\t'))
        textBuffer.erase(le, 1);
    textBuffer.insert(le, 1, ' ');
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

// ── comment toggle ──

std::string Application::commentToken() const {
    std::string lang = syntax_->languageName();
    if (lang=="Python"||lang=="ShellScript"||lang=="Ruby"||lang=="R"||lang=="TOML"||lang=="YAML") return "#";
    if (lang=="SQL"||lang=="Lua") return "--";
    return "//";
}

void Application::toggleLineComment() {
    pushUndo();
    std::string token = commentToken();
    bool isHTML = (token == "<!--");
    auto& sel = selections_[0];
    size_t a = sel.hasSelection() ? lineStart(sel.min()) : lineStart(sel.cursor);
    size_t b = sel.hasSelection() ? lineEnd(sel.max()) : lineEnd(sel.cursor);
    size_t la = lineOfPos(a), lb = lineOfPos(b);
    bool allCommented = true;
    for (size_t ln = la; ln <= lb; ++ln) {
        size_t ls = lineStartForLine(ln), le = lineEnd(ls);
        size_t first = ls; while (first < le && (textBuffer[first]==' '||textBuffer[first]=='\t')) ++first;
        if (isHTML) { if (first+3 >= le || textBuffer.compare(first,4,"<!--")!=0) allCommented = false; }
        else { if (first+token.size() > le || textBuffer.compare(first,token.size(),token)!=0) allCommented = false; }
    }
    for (size_t ln = la; ln <= lb; ++ln) {
        size_t ls = lineStartForLine(ln), le = lineEnd(ls);
        size_t first = ls; while (first < le && (textBuffer[first]==' '||textBuffer[first]=='\t')) ++first;
        if (allCommented) {
            if (isHTML) { textBuffer.erase(first, 4); size_t ce = le - 4; while (ce > first && textBuffer.compare(ce,3,"-->")!=0) --ce; if (ce > first) textBuffer.erase(ce, 3); }
            else textBuffer.erase(first, token.size());
        } else {
            if (isHTML) textBuffer.insert(first, "<!-- ");
            else textBuffer.insert(first, token + " ");
        }
    }
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::toggleBlockComment() {
    auto& sel = selections_[0];
    if (!sel.hasSelection()) { sel.anchor = lineStart(sel.cursor); sel.cursor = lineEnd(sel.cursor); }
    pushUndo();
    size_t a = sel.min(), b = sel.max();
    std::string lang = syntax_->languageName();
    std::string open = (lang=="HTML"||lang=="XML") ? "<!--" : "/*";
    std::string close = (lang=="HTML"||lang=="XML") ? "-->" : "*/";
    textBuffer.insert(b, close);
    textBuffer.insert(a, open);
    sel.anchor = a; sel.cursor = b + open.size() + close.size();
    dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
}

// ── bookmarks ──

void Application::toggleBookmark() {
    int line = (int)lineOfPos(selections_[0].cursor);
    if (bookmarks_.count(line)) bookmarks_.erase(line); else bookmarks_.insert(line);
}
void Application::nextBookmark() {
    if (bookmarks_.empty()) return;
    int cur = (int)lineOfPos(selections_[0].cursor);
    auto it = bookmarks_.upper_bound(cur);
    if (it == bookmarks_.end()) it = bookmarks_.begin();
    selections_[0].anchor = selections_[0].cursor = lineStartForLine(*it);
    ensureCursorVisible();
}
void Application::prevBookmark() {
    if (bookmarks_.empty()) return;
    int cur = (int)lineOfPos(selections_[0].cursor);
    auto it = bookmarks_.lower_bound(cur);
    if (it == bookmarks_.begin()) it = bookmarks_.end();
    --it;
    selections_[0].anchor = selections_[0].cursor = lineStartForLine(*it);
    ensureCursorVisible();
}
void Application::clearBookmarks() { bookmarks_.clear(); }

// ── convert case ──

void Application::ensureWordSel() {
    auto& sel = selections_[0];
    if (!sel.hasSelection()) {
        size_t p = sel.cursor, ls = lineStart(p), le = lineEnd(p);
        size_t ws = p; while (ws > ls && (isalnum(textBuffer[ws-1])||textBuffer[ws-1]=='_')) --ws;
        size_t we = p; while (we < le && (isalnum(textBuffer[we])||textBuffer[we]=='_')) ++we;
        if (we > ws) { sel.anchor = ws; sel.cursor = we; }
    }
}

void Application::convertCaseUpper() {
    ensureWordSel(); if (!selections_[0].hasSelection()) return;
    pushUndo(); size_t a = selections_[0].min(), b = selections_[0].max();
    for (size_t i = a; i < b; ++i) textBuffer[i] = (char)toupper((unsigned char)textBuffer[i]);
    dirty_ = true;
}
void Application::convertCaseLower() {
    ensureWordSel(); if (!selections_[0].hasSelection()) return;
    pushUndo(); size_t a = selections_[0].min(), b = selections_[0].max();
    for (size_t i = a; i < b; ++i) textBuffer[i] = (char)tolower((unsigned char)textBuffer[i]);
    dirty_ = true;
}
void Application::convertCaseTitle() {
    ensureWordSel(); if (!selections_[0].hasSelection()) return;
    pushUndo(); size_t a = selections_[0].min(), b = selections_[0].max();
    bool ws = true;
    for (size_t i = a; i < b; ++i) {
        if (isalnum((unsigned char)textBuffer[i])) { textBuffer[i] = ws ? (char)toupper((unsigned char)textBuffer[i]) : (char)tolower((unsigned char)textBuffer[i]); ws = false; }
        else ws = true;
    }
    dirty_ = true;
}
void Application::convertCaseSwap() {
    ensureWordSel(); if (!selections_[0].hasSelection()) return;
    pushUndo(); size_t a = selections_[0].min(), b = selections_[0].max();
    for (size_t i = a; i < b; ++i) {
        char c = textBuffer[i];
        if (isupper((unsigned char)c)) textBuffer[i] = (char)tolower((unsigned char)c);
        else if (islower((unsigned char)c)) textBuffer[i] = (char)toupper((unsigned char)c);
    }
    dirty_ = true;
}

// ── auto pair ──

void Application::handleAutoPair(const char* text) {
    if (!autoPair_ || text[1] != 0) { insertText(text); return; }
    char c = text[0];
    auto& sel = selections_[0];
    static const struct { char open, close; } pairs[] = {{'(',')'},{'[',']'},{'{','}'},{'"','"'},{'\'','\''}};
    char close = 0;
    for (auto& p : pairs) if (p.open == c) { close = p.close; break; }
    if (!close) { insertText(text); return; }
    if (sel.hasSelection()) {
        pushUndo();
        size_t a = sel.min(), b = sel.max();
        std::string wrapped = std::string(1,c) + textBuffer.substr(a, b-a) + close;
        textBuffer.replace(a, b-a, wrapped);
        sel.anchor = a + 1; sel.cursor = a + 1 + (b - a);
        dirty_ = true; syntaxDirty_ = true; indentsDirty_ = true;
        return;
    }
    if (sel.cursor < textBuffer.size() && textBuffer[sel.cursor] == close) {
        sel.anchor = sel.cursor = sel.cursor + 1;
        return;
    }
    std::string ins; ins += c; ins += close;
    insertText(ins);
    selections_[0].anchor = selections_[0].cursor = selections_[0].cursor - 1;
}

void Application::closeAllPopups() {
    titlebar_->closeMenuPopup();
    tabDropdownOpen_ = false; tabContextOpen_ = false;
    statusPopup_ = StatusPopup::None;
    acActive_ = false; commandPalette_.active = false;
    hidePopupWindow();
}

void Application::openSettingsFile() {
    std::string settingsPath = SettingsManager::instance().path();
    if (!settingsPath.empty() && fs::exists(settingsPath)) openFile(settingsPath);
}

void Application::buildVisualLineMap(float editorWidth, float gutterWidth) {
    visualLines_.clear();
    if (!wordWrap_) return;
    float wrapWidth = editorWidth - gutterWidth - 16.f;
    if (wrapWidth < 50.f) wrapWidth = 50.f;
    float charW = std::max(1.f, fontAtlas().measureText(" "));
    int maxCols = std::max(10, (int)(wrapWidth / charW));
    size_t nLines = totalLines();
    for (size_t ln = 0; ln < nLines; ++ln) {
        if (isFolded(ln)) continue;
        size_t ls = lineStartForLine(ln), le = lineEnd(ls);
        int lineLen = (int)(le - ls);
        if (lineLen <= maxCols) {
            visualLines_.push_back({(int)ln, 0, lineLen});
            continue;
        }
        int col = 0;
        while (col < lineLen) {
            int remaining = lineLen - col;
            if (remaining <= maxCols) {
                visualLines_.push_back({(int)ln, col, lineLen});
                break;
            }
            // find wrap point at word boundary
            int breakCol = col + maxCols;
            int bestBreak = breakCol;
            for (int i = breakCol; i > col + maxCols / 2; --i) {
                char c = textBuffer[ls + i];
                if (c == ' ' || c == '\t') { bestBreak = i + 1; break; }
            }
            visualLines_.push_back({(int)ln, col, bestBreak});
            col = bestBreak;
        }
    }
}

// ── events ──

void Application::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { running_ = false; return; }
        if (popupWin_) {
            Uint32 popupId = SDL_GetWindowID(popupWin_);
            if (e.type == SDL_MOUSEMOTION && e.motion.windowID == popupId) {
                e.motion.x += static_cast<Sint32>(popupMainX_);
                e.motion.y += static_cast<Sint32>(popupMainY_);
            } else if ((e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) && e.button.windowID == popupId) {
                e.button.x += static_cast<Sint32>(popupMainX_);
                e.button.y += static_cast<Sint32>(popupMainY_);
            }
        }
        if (e.type == SDL_WINDOWEVENT && e.window.windowID == SDL_GetWindowID(window_) && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            if (inOsDialog_) continue;
            bool focusMovedToPopup = false;
            if (popupWin_ && (SDL_GetWindowFlags(popupWin_) & SDL_WINDOW_SHOWN)) {
                int gx = 0, gy = 0;
                SDL_GetGlobalMouseState(&gx, &gy);
                focusMovedToPopup = (SDL_GetWindowFlags(popupWin_) & SDL_WINDOW_INPUT_FOCUS) != 0
                    || (gx >= popupScreenX_ && gx < popupScreenX_ + (int)popupMainW_ && gy >= popupScreenY_ && gy < popupScreenY_ + (int)popupMainH_);
            }
            if (focusMovedToPopup) continue;
            titlebar_->closeMenuPopup();
            tabDropdownOpen_ = false; tabContextOpen_ = false; statusPopup_ = StatusPopup::None; acActive_ = false; commandPalette_.active = false;
            hidePopupWindow();
            continue;
        }
        if (popupWin_ && e.type == SDL_WINDOWEVENT && e.window.windowID == SDL_GetWindowID(popupWin_) && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            SDL_Window* focused = SDL_GetKeyboardFocus();
            if (focused != window_ && focused != popupWin_) {
                titlebar_->closeMenuPopup();
                tabDropdownOpen_ = false; tabContextOpen_ = false; statusPopup_ = StatusPopup::None; acActive_ = false; commandPalette_.active = false;
                hidePopupWindow();
            }
            continue;
        }
        if (closeConfirmOpen_) {
            int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
            float mw = 420.f, mh = 118.f, mx0 = ((float)ww - mw) / 2.f, my0 = ((float)wh - mh) / 2.f;
            float by = my0 + 76.f, bw = 96.f, bh = 26.f;
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { closeConfirmOpen_ = false; continue; }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
                float mx = (float)e.button.x, my = (float)e.button.y;
                auto inButton = [&](float x) { return mx >= x && mx <= x + bw && my >= by && my <= by + bh; };
                if (inButton(mx0 + 72.f)) {
                    size_t idx = closeConfirmIndex_;
                    closeConfirmOpen_ = false;
                    switchToTab(idx);
                    saveFile();
                    if (!dirty_) closeTabNow(activeTab_);
                } else if (inButton(mx0 + 182.f)) {
                    size_t idx = closeConfirmIndex_;
                    closeConfirmOpen_ = false;
                    closeTabNow(idx);
                } else if (inButton(mx0 + 292.f) || (mx < mx0 || mx > mx0 + mw || my < my0 || my > my0 + mh)) closeConfirmOpen_ = false;
                continue;
            }
            continue;
        }
        if (e.type == SDL_KEYDOWN) {
            cursorLastInputTicks_ = SDL_GetTicks();
            auto mod = e.key.keysym.mod;
            if (e.key.keysym.sym == SDLK_x && (mod & KMOD_SHIFT) && !(mod & KMOD_CTRL) && !(mod & KMOD_ALT)) {
                debugFpsVisible_ = !debugFpsVisible_;
                continue;
            }
        }
        if (e.type == SDL_TEXTINPUT) cursorLastInputTicks_ = SDL_GetTicks();
        // find mode input
        if (find_.active && e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod; auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { find_.active = false; acActive_ = false; continue; }
            if (sym == SDLK_TAB && find_.replaceActive) { findFocus_ = 1 - findFocus_; continue; }
            if (sym == SDLK_RETURN) {
                if (findFocus_ == 1 && find_.replaceActive) {
                    doReplace();
                } else if (!find_.matches.empty()) {
                    if (mod & KMOD_SHIFT) find_.currentMatch = (find_.currentMatch + find_.matches.size() - 1) % find_.matches.size();
                    else find_.currentMatch = (find_.currentMatch + 1) % find_.matches.size();
                    selections_[0].anchor = selections_[0].cursor = find_.matches[find_.currentMatch];
                    ensureCursorVisible();
                }
                continue;
            }
            if ((mod & KMOD_CTRL) && (sym == SDLK_f || sym == SDLK_h || sym == SDLK_r)) continue;
            if (sym == SDLK_BACKSPACE) {
                if (findFocus_ == 1 && find_.replaceActive) { if (!find_.replace.empty()) { auto it = find_.replace.end(); --it; while (it != find_.replace.begin() && (*it & 0xC0) == 0x80) --it; find_.replace.erase(it, find_.replace.end()); } }
                else { if (!find_.query.empty()) { auto it = find_.query.end(); --it; while (it != find_.query.begin() && (*it & 0xC0) == 0x80) --it; find_.query.erase(it, find_.query.end()); findAllMatches(); } }
                continue;
            }
            if ((mod & KMOD_ALT) && sym == SDLK_r) { find_.regex = !find_.regex; findAllMatches(); continue; }
            if ((mod & KMOD_ALT) && sym == SDLK_c) { find_.caseSensitive = !find_.caseSensitive; findAllMatches(); continue; }
            if ((mod & KMOD_ALT) && sym == SDLK_w) { find_.wholeWord = !find_.wholeWord; findAllMatches(); continue; }
            continue;
        }
        if (find_.active && e.type == SDL_TEXTINPUT) {
            if (findFocus_ == 1 && find_.replaceActive) find_.replace += e.text.text;
            else { find_.query += e.text.text; findAllMatches(); }
            continue;
        }
        if (commandPalette_.active && e.type == SDL_KEYDOWN) {
            auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { commandPalette_.active = false; continue; }
            auto scrollPaletteSelection = [&]() {
                if (commandPalette_.selected < commandPalette_.scroll) commandPalette_.scroll = commandPalette_.selected;
                if (commandPalette_.selected >= commandPalette_.scroll + 12) commandPalette_.scroll = commandPalette_.selected - 11;
                int maxScroll = std::max(0, (int)commandPalette_.results.size() - 12);
                if (commandPalette_.scroll < 0) commandPalette_.scroll = 0;
                if (commandPalette_.scroll > maxScroll) commandPalette_.scroll = maxScroll;
            };
            if (sym == SDLK_RETURN) {
                if (commandPalette_.selected >= 0 && commandPalette_.selected < (int)commandPalette_.results.size()) {
                    int idx = commandPalette_.results[commandPalette_.selected];
                    commandPalette_.active = false;
                    executePaletteCommand(idx);
                }
                continue;
            }
            if (sym == SDLK_UP) { if (commandPalette_.selected > 0) --commandPalette_.selected; scrollPaletteSelection(); continue; }
            if (sym == SDLK_DOWN) { if (commandPalette_.selected < (int)commandPalette_.results.size() - 1) ++commandPalette_.selected; scrollPaletteSelection(); continue; }
            if (sym == SDLK_BACKSPACE && !commandPalette_.query.empty()) {
                auto it = commandPalette_.query.end(); --it;
                while (it != commandPalette_.query.begin() && (*it & 0xC0) == 0x80) --it;
                commandPalette_.query.erase(it, commandPalette_.query.end()); updateCommandPalette(); continue;
            }
            continue;
        }
        if (commandPalette_.active && e.type == SDL_TEXTINPUT) { commandPalette_.query += e.text.text; updateCommandPalette(); continue; }
        // goto mode
        if (goto_.active && e.type == SDL_KEYDOWN) {
            auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { goto_.active = false; continue; }
            if (sym == SDLK_RETURN) {
                if (!goto_.items.empty() && goto_.selected >= 0 && goto_.selected < (int)goto_.items.size()) {
                    if (gotoMode_ == Lines) { goto_.active = false; ensureCursorVisible(); continue; }
                    if (gotoMode_ == Symbols) {
                        // jump to the symbol line
                        goto_.active = false; ensureCursorVisible(); continue;
                    }
                    if (gotoMode_ == Words) {
                        // jump to word occurrence
                        size_t pos = textBuffer.find(goto_.items[goto_.selected]);
                        if (pos != std::string::npos) { selections_[0].anchor = selections_[0].cursor = pos; ensureCursorVisible(); }
                        goto_.active = false; continue;
                    }
                    // files — check if it's an open tab or sidebar file
                    bool goto_next = false;
                    for (auto& tab : tabs_) {
                        if (tab.fileName == goto_.items[goto_.selected]) {
                            size_t idx = &tab - tabs_.data();
                            switchToTab(idx);
                            goto_.active = false;
                            goto_next = true; break;
                        }
                    }
                    if (goto_next) { goto_next = false; continue; }
                    if (!sidebarRoot_.empty()) {
                        try {
                            for (auto& entry : fs::recursive_directory_iterator(sidebarRoot_)) {
                                if (!entry.is_directory() && entry.path().filename().string() == goto_.items[goto_.selected]) {
                                    openFile(entry.path().string()); break;
                                }
                            }
                        } catch (...) {}
                    }
                }
                goto_.active = false; continue;
            }
            if (sym == SDLK_UP) { if (goto_.selected > 0) --goto_.selected; continue; }
            if (sym == SDLK_DOWN) { if (goto_.selected < (int)goto_.items.size() - 1) ++goto_.selected; continue; }
            if (sym == SDLK_BACKSPACE && !goto_.query.empty()) {
                auto it = goto_.query.end(); --it;
                while (it != goto_.query.begin() && (*it & 0xC0) == 0x80) --it;
                goto_.query.erase(it, goto_.query.end()); updateGotoResults(); continue;
            }
            continue;
        }
        if (goto_.active && e.type == SDL_TEXTINPUT) { goto_.query += e.text.text; updateGotoResults(); continue; }
        // menu bar
        int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
        if (titlebar_->handleEvent(e, window_)) continue;
        { int tbww,tbwh; SDL_GL_GetDrawableSize(window_,&tbww,&tbwh); if (handleTabBarEvent(e,(float)tbww,titlebar_->height())) continue; }
        if (commandPalette_.active) {
            float ow = 560.f, ox = ((float)ww - ow) / 2.f, oy = titlebar_->height() + tabBarH_ + 36.f;
            float rowH = 24.f, listY = oy + 38.f, listH = 12.f * rowH;
            int maxScroll = std::max(0, (int)commandPalette_.results.size() - 12);
            if (e.type == SDL_MOUSEMOTION) {
                float mx = (float)e.motion.x, my = (float)e.motion.y;
                commandPalette_.hover = -1;
                if (mx >= ox && mx <= ox + ow && my >= listY && my < listY + listH) {
                    int row = (int)((my - listY) / rowH);
                    int idx = commandPalette_.scroll + row;
                    if (idx >= 0 && idx < (int)commandPalette_.results.size()) {
                        commandPalette_.hover = idx;
                        commandPalette_.selected = idx;
                    }
                }
                continue;
            }
            if (e.type == SDL_MOUSEWHEEL) {
                commandPalette_.scroll -= e.wheel.y * 3;
                if (commandPalette_.scroll < 0) commandPalette_.scroll = 0;
                if (commandPalette_.scroll > maxScroll) commandPalette_.scroll = maxScroll;
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
                float mx = (float)e.button.x, my = (float)e.button.y;
                if (mx >= ox && mx <= ox + ow && my >= listY && my < listY + listH) {
                    int row = (int)((my - listY) / rowH);
                    int resultIndex = commandPalette_.scroll + row;
                    if (resultIndex >= 0 && resultIndex < (int)commandPalette_.results.size()) {
                        int commandIndex = commandPalette_.results[resultIndex];
                        commandPalette_.active = false;
                        executePaletteCommand(commandIndex);
                    }
                } else if (mx < ox || mx > ox + ow || my < oy || my > oy + 330.f) {
                    commandPalette_.active = false;
                }
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL) continue;
        }
        auto statusPopupWidth = [&]() {
            float popW = 240.f;
            if (statusPopup_ == StatusPopup::Indent) {
                const char* indentItems[] = {"Indent Using Spaces","Indent Using Tabs","Tab Width: 2","Tab Width: 4","Tab Width: 8","Convert Indentation to Spaces","Convert Indentation to Tabs","Detect Indentation"};
                for (auto item : indentItems) popW = std::max(popW, fontAtlas().measureText(item) + 48.f);
            } else if (statusPopup_ == StatusPopup::Syntax) {
                int rows = std::min(syntaxLangCount - popupScroll_, 18);
                for (int row = 0; row < rows; ++row) {
                    int i = popupScroll_ + row;
                    if (i >= 0 && i < syntaxLangCount) popW = std::max(popW, fontAtlas().measureText(syntaxLanguages[i]) + 48.f);
                }
            }
            return popW;
        };
        if (statusPopup_ != StatusPopup::None) {
            if (e.type == SDL_MOUSEMOTION) {
                int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
                float popW = statusPopupWidth(), popH = itemCount * 24.f + 4.f;
                if (e.motion.x >= popupX_ && e.motion.x <= popupX_ + popW && e.motion.y >= popupY_ && e.motion.y <= popupY_ + popH) {
                    int idx = (int)std::floor((e.motion.y - popupY_ - 2.f) / 24.f);
                    if (idx < 0) idx = 0;
                    if (idx >= itemCount) idx = itemCount - 1;
                    popupSelected_ = idx;
                } else popupSelected_ = -1;
                continue;
            }
            if (e.type == SDL_MOUSEWHEEL) {
                if (statusPopup_ == StatusPopup::Syntax) {
                    popupScroll_ -= e.wheel.y;
                    int maxScroll = std::max(0, syntaxLangCount - 18);
                    if (popupScroll_ < 0) popupScroll_ = 0;
                    if (popupScroll_ > maxScroll) popupScroll_ = maxScroll;
                }
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
                float mx = (float)e.button.x, my = (float)e.button.y;
                int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
                float popW = statusPopupWidth(), popH = itemCount * 24.f + 4.f;
                if (mx >= popupX_ && mx <= popupX_ + popW && my >= popupY_ && my <= popupY_ + popH) {
                    int idx = (int)std::floor((my - popupY_ - 2.f) / 24.f);
                    if (statusPopup_ == StatusPopup::Syntax) idx += popupScroll_;
                    if (idx >= 0 && idx < (statusPopup_ == StatusPopup::Indent ? 8 : syntaxLangCount)) {
                        if (statusPopup_ == StatusPopup::Indent) {
                            if (idx == 0) { useTabs_ = false; syntax_->setUseTabs(false); }
                            else if (idx == 1) { useTabs_ = true; syntax_->setUseTabs(true); }
                            else if (idx == 2) { tabSize_ = 2; syntax_->setTabSize(2); }
                            else if (idx == 3) { tabSize_ = 4; syntax_->setTabSize(4); }
                            else if (idx == 4) { tabSize_ = 8; syntax_->setTabSize(8); }
                            else if (idx == 5) convertIndentation(true);
                            else if (idx == 6) convertIndentation(false);
                            else if (idx == 7) guessIndent();
                        } else {
                            syntax_->setLanguageByName(syntaxLanguages[idx]);
                            syntaxLangIndex_ = idx;
                            syntaxDirty_ = true; indentsDirty_ = true;
                            syntax_->parse(textBuffer);
                        }
                    }
                }
                statusPopup_ = StatusPopup::None;
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) continue;
        }
        if ((goto_.active || commandPalette_.active || (acActive_ && !acItems_.empty())) &&
            (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL)) {
            continue;
        }
        if (find_.active && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
            float sbH = statusbar_->height();
            float rowH = 30.f, barH = find_.replaceActive ? rowH * 2.f + 2.f : rowH;
            float barY = (float)wh - sbH - barH;
            float mx = (float)e.button.x, my = (float)e.button.y;
            if (my >= barY && my < barY + barH) {
                float panelW = (float)ww;
                float fieldX = 108.f;
                float rightButtonsW = find_.replaceActive ? 286.f : 164.f;
                float fieldW = std::max(120.f, panelW - fieldX - rightButtonsW);
                float fieldH = 22.f;
                float fy = barY + (rowH - fieldH) / 2.f;
                auto inRect = [&](float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; };
                if (inRect(8.f, fy, 28.f, fieldH)) { find_.regex = !find_.regex; findAllMatches(); continue; }
                if (inRect(40.f, fy, 28.f, fieldH)) { find_.caseSensitive = !find_.caseSensitive; findAllMatches(); continue; }
                if (inRect(72.f, fy, 28.f, fieldH)) { find_.wholeWord = !find_.wholeWord; findAllMatches(); continue; }
                if (inRect(fieldX, fy, fieldW, fieldH)) { findFocus_ = 0; continue; }
                float prevX = fieldX + fieldW + 8.f;
                float nextX = prevX + 62.f;
                float closeX = panelW - 30.f;
                if (inRect(prevX, fy, 56.f, fieldH)) {
                    if (!find_.matches.empty()) {
                        find_.currentMatch = (find_.currentMatch + find_.matches.size() - 1) % find_.matches.size();
                        selections_[0].anchor = selections_[0].cursor = find_.matches[find_.currentMatch];
                        ensureCursorVisible();
                    }
                    continue;
                }
                if (inRect(nextX, fy, 56.f, fieldH)) {
                    if (!find_.matches.empty()) {
                        find_.currentMatch = (find_.currentMatch + 1) % find_.matches.size();
                        selections_[0].anchor = selections_[0].cursor = find_.matches[find_.currentMatch];
                        ensureCursorVisible();
                    }
                    continue;
                }
                if (inRect(closeX, fy, 22.f, fieldH)) { find_.active = false; continue; }
                if (find_.replaceActive) {
                    float ry = barY + rowH + 2.f;
                    float rfy = ry + (rowH - fieldH) / 2.f;
                    if (inRect(8.f, rfy, 28.f, fieldH)) continue;
                    if (inRect(40.f, rfy, 28.f, fieldH)) continue;
                    if (inRect(fieldX, rfy, fieldW, fieldH)) { findFocus_ = 1; continue; }
                    float replaceX = fieldX + fieldW + 8.f;
                    float replaceAllX = replaceX + 82.f;
                    if (inRect(replaceX, rfy, 76.f, fieldH)) { doReplace(); continue; }
                    if (inRect(replaceAllX, rfy, 96.f, fieldH)) { doReplaceAll(); continue; }
                }
                continue;
            }
        }
        if (handleSidebarEvent(e, (float)wh, titlebar_->height() + tabBarH_, statusbar_->height())) continue;
        // find bar click handling
        if (find_.active && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
            float sbH = statusbar_->height();
            float rowH = 30.f, barH = find_.replaceActive ? rowH * 2.f + 2.f : rowH;
            float barY = (float)wh - sbH - barH;
            float mx = (float)e.button.x, my = (float)e.button.y;
            if (my >= barY && my < barY + barH) {
                float editorRight = (float)ww - (minimapVisible_ ? 100.f : 0.f);
                float fieldW = editorRight - 220.f;
                float btnX = fieldW + 16.f;
                float fieldH = 22.f, fy = barY + (rowH - fieldH) / 2.f;
                // toggle buttons
                if (my >= fy && my < fy + fieldH) {
                    if (mx >= btnX && mx < btnX + 28.f) { find_.regex = !find_.regex; findAllMatches(); continue; }
                    btnX += 32.f;
                    if (mx >= btnX && mx < btnX + 28.f) { find_.caseSensitive = !find_.caseSensitive; findAllMatches(); continue; }
                    btnX += 32.f;
                    if (mx >= btnX && mx < btnX + 28.f) { find_.wholeWord = !find_.wholeWord; findAllMatches(); continue; }
                }
                // close X
                if (mx >= editorRight - 22.f && mx < editorRight && my >= barY && my < barY + rowH) { find_.active = false; continue; }
                // replace row buttons
                if (find_.replaceActive && my >= barY + rowH) {
                    float rbx = fieldW + 16.f + 96.f;
                    float rbw = fontAtlas().measureText("Replace");
                    if (mx >= rbx && mx < rbx + rbw + 10.f) { doReplace(); continue; }
                    rbx += rbw + 14.f;
                    if (mx >= rbx) { doReplaceAll(); continue; }
                }
                // click in field area focuses
                if (mx >= 8.f && mx < fieldW + 8.f) {
                    findFocus_ = (my < barY + rowH) ? 0 : 1;
                    if (!find_.replaceActive) findFocus_ = 0;
                }
                continue;
            }
        }
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED)) {
            SDL_GL_GetDrawableSize(window_, &ww, &wh); GLRenderer::resize(ww, wh); titlebar_->layout(ww);
        }
        else if (e.type == SDL_MOUSEMOTION) {
            mouseX_ = e.motion.x; mouseY_ = e.motion.y;
            float tbH = titlebar_->height() + tabBarH_;
            float sbH = statusbar_->height();
            float findPanelH = find_.active ? (find_.replaceActive ? 62.f : 30.f) : 0.f;
            float lineStep = fontAtlas().lineHeight();
            float viewH = (float)wh - tbH - sbH - findPanelH;
            float contentH = totalLines() * lineStep;
            float scrollPad = scrollPastEnd_ ? lineStep * 5.f : 0.f;
            float maxScroll = contentH + scrollPad > viewH ? contentH + scrollPad - viewH : 0.f;
            float mmX = (float)ww - (minimapVisible_ ? minimap_->width() : 0.f);
            float sbX = mmX - 10.f;
            scrollbarHovered_ = !minimapVisible_ && e.motion.x >= (int)sbX && e.motion.x < (int)mmX && e.motion.y >= (int)tbH && e.motion.y < wh - (int)(sbH + findPanelH);
            auto clampScroll = [&] { if (scrollY_ < 0.f) scrollY_ = 0.f; if (scrollY_ > maxScroll) scrollY_ = maxScroll; };
            float editorLeft = (sidebarVisible_ ? sidebarWidth_ : 0.f) + gutter_->width() + 8.f;
            float editorRight = mmX - (!minimapVisible_ ? 10.f : 0.f);
            float editorW = editorRight - editorLeft;
            computeMaxLineWidth();
            float maxScrollX = (!wordWrap_ && editorW > 0.f) ? std::max(0.f, maxLineWidth_ - editorW) : 0.f;
            bool hVisible = maxScrollX > 0.f;
            horizontalScrollbarHovered_ = hVisible && e.motion.x >= (int)editorLeft && e.motion.x < (int)editorRight && e.motion.y >= wh - (int)(sbH + findPanelH) - 10 && e.motion.y < wh - (int)(sbH + findPanelH);
            if (scrollbarDragging_) {
                float thumbH = contentH > 0.f ? viewH * (viewH / contentH) : viewH;
                if (thumbH > viewH) thumbH = viewH;
                if (thumbH < 20.f) thumbH = 20.f;
                float usable = viewH - thumbH;
                float rel = usable > 0.f ? ((float)e.motion.y - tbH - scrollbarDragOffset_) / usable : 0.f;
                scrollY_ = rel * maxScroll; clampScroll(); continue;
            }
            if (horizontalScrollbarDragging_) {
                float regionW = editorW;
                float thumbW = std::max(20.f, regionW * (regionW / maxLineWidth_));
                if (thumbW > regionW) thumbW = regionW;
                float usable = regionW - thumbW;
                float rel = usable > 0.f ? ((float)e.motion.x - editorLeft - horizontalScrollbarDragOffset_) / usable : 0.f;
                scrollX_ = rel * maxScrollX;
                if (scrollX_ < 0.f) scrollX_ = 0.f;
                if (scrollX_ > maxScrollX) scrollX_ = maxScrollX;
                continue;
            }
            if (minimapDragging_) {
                float minimapH = viewH;
                float scale = minimapH > 0.f ? (contentH + scrollPad) / minimapH : 1.f;
                scrollY_ = minimapDragStartScrollY_ + ((float)e.motion.y - minimapDragStartY_) * scale;
                clampScroll();
                continue;
            }
            if (selecting_) {
                if (boxSelActive_) {
                    float sidebarOffset = sidebarVisible_ ? sidebarWidth_ : 0.f;
                    float gutterW = sidebarOffset + gutter_->width(), textX = gutterW + 8.0f;
                    float clickY = (float)e.motion.y - tbH + scrollY_;
                    int endLine = clickY > 0.f ? (int)std::floor(clickY / lineStep) : 0;
                    if (endLine >= (int)totalLines()) endLine = (int)totalLines() - 1;
                    float charW = std::max(1.f, fontAtlas().measureText(" "));
                    boxSelEndCol_ = std::max(0, (int)std::floor(((float)e.motion.x - textX + scrollX_) / charW));
                    boxSelEndLine_ = endLine;
                    int minLine = std::min(boxSelStartLine_, boxSelEndLine_);
                    int maxLine = std::max(boxSelStartLine_, boxSelEndLine_);
                    int minCol = std::min(boxSelStartCol_, boxSelEndCol_);
                    int maxCol = std::max(boxSelStartCol_, boxSelEndCol_);
                    selections_.clear();
                    for (int ln = minLine; ln <= maxLine; ++ln) {
                        size_t ls = lineStartForLine(ln), le = lineEnd(ls);
                        std::string_view lt(textBuffer.data() + ls, le - ls);
                        size_t colA = 0, colB = 0; int vc = 0;
                        for (size_t i = 0; i < lt.size();) {
                            uint32_t cp = (uint8_t)lt[i]; int b = 1;
                            if (cp>=0xF0) b=4; else if (cp>=0xE0) b=3; else if (cp>=0xC0) b=2;
                            int step = (cp=='\t') ? tabSize_ : 1;
                            if (vc == minCol) colA = i;
                            if (vc + step > maxCol) { colB = i; break; }
                            vc += step; i += b;
                        }
                        if (colA == 0 && minCol > 0) colA = lt.size();
                        if (colB == 0) colB = lt.size();
                        selections_.emplace_back(ls + colA, ls + colB);
                    }
                    continue;
                }
                float sidebarOffset = sidebarVisible_ ? sidebarWidth_ : 0.f;
                float gutterW = sidebarOffset + gutter_->width(), textX = gutterW + 8.0f;
                float clickY = (float)e.motion.y - tbH + scrollY_;
                size_t clickLine = clickY > 0.f ? (size_t)std::floor(clickY / lineStep) : 0;
                if (clickLine >= totalLines()) clickLine = totalLines() - 1;
                size_t ls = lineStartForLine(clickLine), le = lineEnd(ls);
                std::string_view lt(textBuffer.data() + ls, le - ls);
                float charW = std::max(1.f, fontAtlas().measureText(" "));
                int targetCol = std::max(0, (int)std::floor(((float)e.motion.x - textX + scrollX_) / charW));
                size_t col = 0; int visualCol = 0;
                for (size_t i = 0; i < lt.size();) {
                    uint32_t cp = (uint8_t)lt[i]; int b = 1;
                    if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                    int step = (cp == '\t') ? tabSize_ : 1;
                    if (visualCol + step > targetCol) break;
                    visualCol += step; col += b; i += b;
                }
                selections_[0].cursor = ls + col;
                ensureCursorVisible();
                continue;
            }
            if (statusPopup_ != StatusPopup::None) {
            int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
            float popW = 240.f, popH = itemCount * 24.f + 4.f;
            if (e.motion.x >= popupX_ && e.motion.x <= popupX_ + popW && e.motion.y >= popupY_ && e.motion.y <= popupY_ + popH) {
                int idx = (int)std::floor((e.motion.y - popupY_ - 2.f) / 24.f);
                if (idx < 0) idx = 0;
                if (idx >= itemCount) idx = itemCount - 1;
                popupSelected_ = idx;
            } else popupSelected_ = -1;
            }
        }
        else if (e.type == SDL_MOUSEWHEEL) {
            if (statusPopup_ == StatusPopup::Syntax) {
                popupScroll_ -= e.wheel.y;
                int maxScroll = std::max(0, syntaxLangCount - 18);
                if (popupScroll_ < 0) popupScroll_ = 0;
                if (popupScroll_ > maxScroll) popupScroll_ = maxScroll;
                continue;
            }
            float lineStep = fontAtlas().lineHeight();
            float contentH = totalLines() * lineStep + 100;
            SDL_GL_GetDrawableSize(window_, &ww, &wh);
            if ((e.wheel.direction == SDL_MOUSEWHEEL_NORMAL) && (SDL_GetModState() & KMOD_SHIFT) && !wordWrap_) {
                float mmW = minimapVisible_ ? minimap_->width() : 0.f;
                float editorW = (float)ww - mmW - 10.f - ((sidebarVisible_ ? sidebarWidth_ : 0.f) + gutter_->width() + 8.f);
                computeMaxLineWidth();
                float maxScrollX = std::max(0.f, maxLineWidth_ - editorW);
                scrollX_ -= e.wheel.y * 64.f;
                if (scrollX_ < 0.f) scrollX_ = 0.f;
                if (scrollX_ > maxScrollX) scrollX_ = maxScrollX;
                continue;
            }
            scrollY_ -= e.wheel.y * lineStep * 3;
            float maxS = contentH - (wh - titlebar_->height() - tabBarH_ - statusbar_->height());
            if (maxS < 0) maxS = 0;
            if (scrollY_ > maxS) scrollY_ = maxS;
            if (scrollY_ < 0) scrollY_ = 0;
        }
        else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
            SDL_GL_GetDrawableSize(window_, &ww, &wh);
            float mx = (float)e.button.x, my = (float)e.button.y;
            float tbH = titlebar_->height() + tabBarH_;
            float lineStep = fontAtlas().lineHeight();
            float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
            float sidebarOffset = sidebarVisible_ ? sidebarWidth_ : 0.f;
            float gutterW = sidebarOffset + gutter_->width();
            float textX = gutterW + 8.0f;
            float sbH = statusbar_->height();
            float findPanelH = find_.active ? (find_.replaceActive ? 62.f : 30.f) : 0.f;
            float fww = (float)ww, fwh = (float)wh;
            float mmX = fww - (minimapVisible_ ? minimap_->width() : 0.f);
            float sbX = mmX - 10.f;
            float viewH = fwh - tbH - sbH - findPanelH;
            float contentH = totalLines() * lineStep;
            float scrollPad = scrollPastEnd_ ? lineStep * 5.f : 0.f;
            float maxScroll = contentH + scrollPad > viewH ? contentH + scrollPad - viewH : 0.f;
            auto clampScroll = [&] { if (scrollY_ < 0.f) scrollY_ = 0.f; if (scrollY_ > maxScroll) scrollY_ = maxScroll; };
            if (!minimapVisible_ && mx >= sbX && mx < mmX && my >= tbH && my < fwh - sbH - findPanelH) {
                float thumbH = contentH > 0.f ? viewH * (viewH / contentH) : viewH;
                if (thumbH > viewH) thumbH = viewH;
                if (thumbH < 20.f) thumbH = 20.f;
                float thumbY = maxScroll > 0.f ? tbH + (scrollY_ / maxScroll) * (viewH - thumbH) : tbH;
                scrollbarDragging_ = true;
                scrollbarDragOffset_ = my >= thumbY && my <= thumbY + thumbH ? my - thumbY : thumbH / 2.f;
                float usable = viewH - thumbH;
                float rel = usable > 0.f ? (my - tbH - scrollbarDragOffset_) / usable : 0.f;
                scrollY_ = rel * maxScroll; clampScroll(); continue;
            }
            float editorLeft = (sidebarVisible_ ? sidebarWidth_ : 0.f) + gutter_->width() + 8.f;
            float editorRight = mmX - (!minimapVisible_ ? 10.f : 0.f);
            float editorW = editorRight - editorLeft;
            computeMaxLineWidth();
            float maxScrollX = (!wordWrap_ && editorW > 0.f) ? std::max(0.f, maxLineWidth_ - editorW) : 0.f;
            if (maxScrollX > 0.f && mx >= editorLeft && mx < editorRight && my >= fwh - sbH - findPanelH - 10.f && my < fwh - sbH - findPanelH) {
                float thumbW = std::max(20.f, editorW * (editorW / maxLineWidth_));
                if (thumbW > editorW) thumbW = editorW;
                float thumbX = editorLeft + (scrollX_ / maxScrollX) * (editorW - thumbW);
                horizontalScrollbarDragging_ = true;
                horizontalScrollbarDragOffset_ = mx >= thumbX && mx <= thumbX + thumbW ? mx - thumbX : thumbW / 2.f;
                float usable = editorW - thumbW;
                float rel = usable > 0.f ? (mx - editorLeft - horizontalScrollbarDragOffset_) / usable : 0.f;
                scrollX_ = rel * maxScrollX;
                if (scrollX_ < 0.f) scrollX_ = 0.f;
                if (scrollX_ > maxScrollX) scrollX_ = maxScrollX;
                continue;
            }
            if (mx >= mmX && my >= tbH && my < fwh - sbH - findPanelH) {
                float minimapH = viewH;
                float vpH = minimapH * 0.15f;
                float vpTop = tbH + (maxScroll > 0.f ? (scrollY_ / maxScroll) * (minimapH - vpH) : 0.f);
                if (vpTop < tbH) vpTop = tbH;
                if (vpTop + vpH > fwh - sbH - findPanelH) vpTop = fwh - sbH - findPanelH - vpH;
                if (my >= vpTop && my <= vpTop + vpH) {
                    minimapDragging_ = true;
                    minimapPendingJump_ = false;
                    minimapDragStartY_ = my;
                    minimapDragStartScrollY_ = scrollY_;
                } else {
                    minimapPendingJump_ = true;
                    minimapPendingJumpY_ = my;
                }
                continue;
            }
            // status bar popup click
            if (statusPopup_ != StatusPopup::None) {
                int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
                float popW = 240.f, popH = itemCount * 24.f + 4.f;
                if (mx >= popupX_ && mx <= popupX_ + popW && my >= popupY_ && my <= popupY_ + popH) {
                    int idx = (int)std::floor((my - popupY_ - 2.f) / 24.f);
                    if (statusPopup_ == StatusPopup::Syntax) idx += popupScroll_;
                    if (idx >= 0 && idx < (statusPopup_ == StatusPopup::Indent ? 8 : syntaxLangCount)) {
                        if (statusPopup_ == StatusPopup::Indent) {
                            if (idx == 0) { useTabs_ = false; syntax_->setUseTabs(false); }
                            else if (idx == 1) { useTabs_ = true; syntax_->setUseTabs(true); }
                            else if (idx == 2) { tabSize_ = 2; syntax_->setTabSize(2); }
                            else if (idx == 3) { tabSize_ = 4; syntax_->setTabSize(4); }
                            else if (idx == 4) { tabSize_ = 8; syntax_->setTabSize(8); }
                            else if (idx == 5) convertIndentation(true);
                            else if (idx == 6) convertIndentation(false);
                            else if (idx == 7) guessIndent();
                        } else {
                            if (idx < syntaxLangCount) {
                                syntax_->setLanguageByName(syntaxLanguages[idx]);
                                syntaxLangIndex_ = idx;
                                syntaxDirty_ = true; indentsDirty_ = true;
                                syntax_->parse(textBuffer);
                            }
                        }
                    }
                    statusPopup_ = StatusPopup::None;
                    continue;
                }
                statusPopup_ = StatusPopup::None;
                continue;
            }
            // status bar click
            if (my >= fwh - sbH && my < fwh) {
                float editorRight = fww;
                std::string indentLabel = useTabs_ ? ("Tab Size: " + std::to_string(tabSize_)) : ("Spaces: " + std::to_string(tabSize_));
                float indentW = fontAtlas().measureText(indentLabel);
                std::string synLabel = syntax_->languageName();
                float synW = fontAtlas().measureText(synLabel);
                float indentX = editorRight - indentW - synW - 32.f - 12.f;
                float synX = editorRight - synW - 12.f;
                if (mx >= indentX && mx <= indentX + indentW + 10.f) {
                    statusPopup_ = StatusPopup::Indent; popupX_ = indentX; popupY_ = fwh - sbH - 8 * 24.f - 4.f; popupSelected_ = 0; popupScroll_ = 0; continue;
                }
                if (mx >= synX && mx <= synX + synW + 10.f) {
                    int visibleLangs = std::min(syntaxLangCount, 18);
                    statusPopup_ = StatusPopup::Syntax; popupX_ = synX; popupY_ = fwh - sbH - visibleLangs * 24.f - 4.f; popupSelected_ = 0; popupScroll_ = 0; continue;
                }
            }
            // find/replace button clicks
            if (find_.active && find_.replaceActive) {
                float barH = 64.f, barY = fwh - sbH - barH;
                float er = fww - (minimapVisible_ ? 100.f : 0.f) - 10.f;
                float bx = er - 280.f;
                if (my >= barY + 26.f && my <= barY + 44.f) {
                    if (mx >= bx && mx <= bx + 72.f) { doReplace(); continue; }
                    if (mx >= bx + 80.f && mx <= bx + 170.f) { doReplaceAll(); continue; }
                }
            }
            // gutter fold click
            if (my > tbH && mx < gutterW) {
                float clickY = my + scrollY_ - textOriginY;
                size_t clickLine = (size_t)(clickY / lineStep);
                if (clickLine < totalLines()) {
                    computeLineIndents();
                    if (isFoldableLine(clickLine) || isFoldStart(clickLine)) { toggleFold(clickLine); continue; }
                }
            }
            if (my > tbH && mx >= gutterW) {
                auto mod = SDL_GetModState();
                float clickY = my - tbH + scrollY_;
                size_t clickLine = clickY > 0.f ? (size_t)std::floor(clickY / lineStep) : 0;
                if (clickLine >= totalLines()) clickLine = totalLines() - 1;
                size_t ls = lineStartForLine(clickLine), le = lineEnd(ls);
                std::string_view lt(textBuffer.data() + ls, le - ls);
                float charW = std::max(1.f, fontAtlas().measureText(" "));
                int targetCol = std::max(0, (int)std::floor((mx - textX + scrollX_) / charW));
                size_t col = 0; int visualCol = 0;
                for (size_t i = 0; i < lt.size(); ) {
                    uint32_t cp = (uint8_t)lt[i]; int b = 1;
                    if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                    int step = (cp == '\t') ? tabSize_ : 1;
                    if (visualCol + step > targetCol) break;
                    visualCol += step; col += b; i += b;
                }
                size_t clickPos = ls + col;
                if (mod & KMOD_CTRL) { selections_.emplace_back(clickPos); }
                else if (mod & KMOD_ALT) {
                    boxSelActive_ = true;
                    int clickLine = (int)lineOfPos(clickPos);
                    boxSelStartLine_ = clickLine; boxSelStartCol_ = (int)col;
                    boxSelEndLine_ = clickLine; boxSelEndCol_ = (int)col;
                    selections_.clear(); selections_.emplace_back(clickPos);
                }
                else if (mod & KMOD_SHIFT) selections_[0].cursor = clickPos;
                else { selections_.clear(); selections_.emplace_back(clickPos); }
                selecting_ = true;
                acActive_ = false;
                desiredCursorX_ = -1.f;
            }
        }
        else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 1) {
            if (minimapPendingJump_) {
                int rw = 0, rh = 0; SDL_GL_GetDrawableSize(window_, &rw, &rh);
                float tbH = titlebar_->height() + tabBarH_;
                float sbH = statusbar_->height();
                float viewH = (float)rh - tbH - sbH;
                float lineStep = fontAtlas().lineHeight();
                float contentH = totalLines() * lineStep;
                float scrollPad = scrollPastEnd_ ? lineStep * 5.f : 0.f;
                float maxScroll = contentH + scrollPad > viewH ? contentH + scrollPad - viewH : 0.f;
                float rel = viewH > 0.f ? (minimapPendingJumpY_ - tbH) / viewH : 0.f;
                if (rel < 0.f) rel = 0.f;
                if (rel > 1.f) rel = 1.f;
                scrollY_ = rel * maxScroll;
            }
            selecting_ = false;
            boxSelActive_ = false;
            minimapDragging_ = false;
            minimapPendingJump_ = false;
            scrollbarDragging_ = false;
            horizontalScrollbarDragging_ = false;
        }
        else if (e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod; auto sym = e.key.keysym.sym;
            if (sym == SDLK_LCTRL || sym == SDLK_RCTRL || sym == SDLK_LSHIFT || sym == SDLK_RSHIFT ||
                sym == SDLK_LALT || sym == SDLK_RALT || sym == SDLK_LGUI || sym == SDLK_RGUI) {
                continue;
            }
            auto& sel = selections_[0];
            bool shift = mod & KMOD_SHIFT, ctrl = mod & KMOD_CTRL;
            // convert to key string for keybinding dispatch
            {
                std::string keyStr;
                if (ctrl) keyStr += "ctrl+";
                if (mod & KMOD_ALT) keyStr += "alt+";
                if (shift && !ctrl) keyStr += "shift+";
                if (ctrl && shift) keyStr += "shift+";
                if (sym >= SDLK_a && sym <= SDLK_z) keyStr += (char)(sym);
                else if (sym >= SDLK_0 && sym <= SDLK_9) keyStr += (char)(sym);
                else if (sym >= SDLK_F1 && sym <= SDLK_F12) keyStr += "f" + std::to_string(sym - SDLK_F1 + 1);
                else if (sym == SDLK_SLASH) keyStr += "/";
                else if (sym == SDLK_BACKSLASH) keyStr += "\\";
                else if (sym == SDLK_SPACE) keyStr += "space";
                else if (sym == SDLK_RETURN) keyStr += "enter";
                else if (sym == SDLK_TAB) keyStr += "tab";
                else if (sym == SDLK_BACKSPACE) keyStr += "backspace";
                else if (sym == SDLK_UP) keyStr += "up";
                else if (sym == SDLK_DOWN) keyStr += "down";
                else if (sym == SDLK_LEFT) keyStr += "left";
                else if (sym == SDLK_RIGHT) keyStr += "right";
                else if (sym == SDLK_HOME) keyStr += "home";
                else if (sym == SDLK_END) keyStr += "end";
                else if (sym == SDLK_PAGEUP) keyStr += "pageup";
                else if (sym == SDLK_PAGEDOWN) keyStr += "pagedown";
                else if (sym == SDLK_INSERT) keyStr += "insert";
                else if (sym == SDLK_DELETE) keyStr += "delete";
                else if (sym == SDLK_BACKQUOTE) keyStr += "`";
                else if (sym == SDLK_MINUS) keyStr += "-";
                else if (sym == SDLK_EQUALS) keyStr += "=";
                else if (sym == SDLK_LEFTBRACKET) keyStr += "[";
                else if (sym == SDLK_RIGHTBRACKET) keyStr += "]";
                else if (sym == SDLK_SEMICOLON) keyStr += ";";
                else if (sym == SDLK_QUOTE) keyStr += "'";
                else if (sym == SDLK_COMMA) keyStr += ",";
                else if (sym == SDLK_PERIOD) keyStr += ".";
                if (!keyStr.empty()) {
                    // try chord dispatch
                    if (pendingCtrlK_) {
                        pendingCtrlK_ = false;
                        if (KeyBindingManager::instance().dispatchChord("ctrl+k", keyStr)) continue;
                    }
                    if (KeyBindingManager::instance().dispatch(keyStr)) continue;
                    // check if this starts a chord
                    if (KeyBindingManager::instance().hasChord(keyStr)) { pendingCtrlK_ = false; /* use as generic chord flag */ continue; }
                }
            }
            if (pendingCtrlK_) {
                pendingCtrlK_ = false;
                if (ctrl && sym == SDLK_1) { foldAll(); continue; }
                if (ctrl && sym == SDLK_j) { unfoldAll(); continue; }
                if (ctrl && sym == SDLK_u) { convertCaseUpper(); continue; }
                if (ctrl && sym == SDLK_l) { convertCaseLower(); continue; }
            }
            if (ctrl && sym == SDLK_q) running_ = false;
            else if (sym == SDLK_ESCAPE) { selections_.clear(); selections_.emplace_back(sel.cursor); find_.active = false; goto_.active = false; commandPalette_.active = false; tabDropdownOpen_ = false; acActive_ = false; }
            else if (ctrl && sym == SDLK_k) { pendingCtrlK_ = true; }
            else if (ctrl && shift && sym == SDLK_UP) { swapLineUp(); }
            else if (ctrl && shift && sym == SDLK_DOWN) { swapLineDown(); }
            else if (ctrl && (mod & KMOD_ALT) && sym == SDLK_UP) {
                int curLine = (int)lineOfPos(selections_[0].cursor);
                if (curLine > 0) {
                    size_t ls = lineStartForLine(curLine - 1);
                    size_t col = std::min(selections_[0].cursor - lineStart(selections_[0].cursor), lineEnd(ls) - ls);
                    selections_.insert(selections_.begin(), SelRange(ls + col));
                }
            }
            else if (ctrl && (mod & KMOD_ALT) && sym == SDLK_DOWN) {
                int curLine = (int)lineOfPos(selections_[0].cursor);
                if (curLine < (int)totalLines() - 1) {
                    size_t ls = lineStartForLine(curLine + 1);
                    size_t col = std::min(selections_[0].cursor - lineStart(selections_[0].cursor), lineEnd(ls) - ls);
                    selections_.emplace_back(ls + col);
                }
            }
            else if (ctrl && shift && sym == SDLK_d) { duplicateLine(); }
            else if (ctrl && shift && sym == SDLK_k) { deleteLine(); }
            else if (ctrl && shift && sym == SDLK_j) { joinLines(); }
            else if (ctrl && !shift && sym == SDLK_SLASH) { toggleLineComment(); }
            else if (ctrl && shift && sym == SDLK_SLASH) { toggleBlockComment(); }
            else if (ctrl && sym == SDLK_a) { sel.anchor = 0; sel.cursor = textBuffer.size(); }
            else if (ctrl && sym == SDLK_c) copySelectionOrLine();
            else if (ctrl && sym == SDLK_x) cutSelectionOrLine();
            else if (ctrl && sym == SDLK_v) pasteClipboard(shift);
            else if (ctrl && sym == SDLK_b) toggleSidebar();
            else if (ctrl && sym == SDLK_z) { if (shift) doRedo(); else doUndo(); }
            else if (ctrl && sym == SDLK_y) doRedo();
            else if (ctrl && sym == SDLK_f) { find_.active = true; find_.replaceActive = false; find_.query.clear(); find_.matches.clear(); findFocus_ = 0; }
            else if (ctrl && sym == SDLK_h) { find_.active = true; find_.replaceActive = true; find_.query.clear(); find_.matches.clear(); findFocus_ = 0; }
            else if (ctrl && sym == SDLK_s) { if (shift) saveFileAs(); else saveFile(); }
            else if (ctrl && shift && sym == SDLK_t) reopenClosedTab();
            else if (ctrl && shift && sym == SDLK_p) commandPalette();
            else if (ctrl && sym == SDLK_p) { goto_.active = true; goto_.query.clear(); goto_.selected = 0; goto_.items.clear(); }
            else if (ctrl && sym == SDLK_o) openFileDialog();
            else if (ctrl && sym == SDLK_F2) { toggleBookmark(); }
            else if (ctrl && shift && sym == SDLK_F2) { clearBookmarks(); }
            else if (sym == SDLK_F2 && (mod & KMOD_SHIFT)) { prevBookmark(); }
            else if (sym == SDLK_F2 && !(mod & KMOD_CTRL)) { nextBookmark(); }
            else if (sym == SDLK_F11 && !(mod & KMOD_SHIFT)) { toggleFullscreen(); }
            else if ((mod & KMOD_ALT) && sym == SDLK_z) { wordWrap_ = !wordWrap_; visualLines_.clear(); }
            else if (ctrl && sym == SDLK_n) newBuffer();
            else if (ctrl && sym == SDLK_w) closeTab(activeTab_);
            else if (ctrl && shift && sym == SDLK_LEFTBRACKET) toggleFold(lineOfPos(sel.cursor));
            else if (ctrl && shift && sym == SDLK_RIGHTBRACKET) {
                size_t line = lineOfPos(sel.cursor);
                int ln = static_cast<int>(line);
                for (auto it = foldedRegions_.begin(); it != foldedRegions_.end(); ++it) {
                    if (it->first == ln || (ln > it->first && ln <= it->second)) { foldedRegions_.erase(it); break; }
                }
            }
            else if (ctrl && sym == SDLK_d) {
                size_t pos = sel.cursor, ls = lineStart(pos), le = lineEnd(ls);
                std::string_view lt(textBuffer.data() + ls, le - ls);
                size_t col = pos - ls, ws = col, we = col;
                while (ws > 0 && (isalnum(lt[ws-1]) || lt[ws-1] == '_')) --ws;
                while (we < lt.size() && (isalnum(lt[we]) || lt[we] == '_')) ++we;
                if (we > ws) {
                    std::string word(lt.substr(ws, we - ws));
                    size_t sf = sel.hasSelection() ? sel.max() : (ls + we);
                    size_t found = textBuffer.find(word, sf);
                    if (found == std::string::npos) found = textBuffer.find(word);
                    if (found != std::string::npos) selections_.emplace_back(found, found + word.size());
                }
            }
            else if (ctrl && sym == SDLK_l) { size_t ls = lineStart(sel.cursor), le = lineEnd(sel.cursor); sel.anchor = ls; sel.cursor = (le < textBuffer.size()) ? le + 1 : le; }
            else if (ctrl && shift && sym == SDLK_l) {
                if (sel.hasSelection()) {
                    size_t a = sel.min(), b = sel.max();
                    std::vector<SelRange> ns; size_t pos = a;
                    while (pos <= b) { size_t le = lineEnd(pos); if (le > b) le = b; ns.emplace_back(pos, le); pos = le + 1; if (pos > b || pos == 0) break; }
                    selections_ = std::move(ns);
                }
            }
            else if (sym == SDLK_BACKSPACE) {
                pushUndo();
                if (sel.hasSelection()) deleteSelection();
                else if (sel.cursor > 0) {
                    size_t pos = sel.cursor; auto it = textBuffer.begin() + pos; --it;
                    while (it != textBuffer.begin() && (*it & 0xC0) == 0x80) --it;
                    size_t ds = it - textBuffer.begin();
                    // auto-pair backspace
                    if (autoPair_ && pos < textBuffer.size()) {
                        char prev = textBuffer[ds], next = textBuffer[pos];
                        if ((prev=='('&&next==')')||(prev=='['&&next==']')||(prev=='{'&&next=='}')||(prev=='"'&&next=='"')||(prev=='\''&&next=='\'')) {
                            textBuffer.erase(ds, 2);
                            sel.anchor = sel.cursor = ds; dirty_ = true; desiredCursorX_ = -1.f;
                            goto did_backspace;
                        }
                    }
                    textBuffer.erase(ds, pos - ds);
                    sel.anchor = sel.cursor = ds; dirty_ = true; desiredCursorX_ = -1.f;
                }
                did_backspace:;
            }
            else if (sym == SDLK_DELETE) {
                pushUndo();
                if (sel.hasSelection()) deleteSelection();
                else if (sel.cursor < textBuffer.size()) {
                    size_t pos = sel.cursor, de = pos + 1;
                    while (de < textBuffer.size() && (textBuffer[de] & 0xC0) == 0x80) ++de;
                    textBuffer.erase(pos, de - pos); dirty_ = true;
                }
            }
            else if (sym == SDLK_RETURN) { if (acActive_) { acceptAutocomplete(); } else { insertText("\n"); } }
            else if (sym == SDLK_TAB) {
                if (acActive_) { acceptAutocomplete(); }
                else if (shift) {
                    pushUndo(); size_t ls = lineStart(sel.cursor); int rm = 0;
                    while (rm < tabSize_ && ls + rm < textBuffer.size() && textBuffer[ls + rm] == ' ') ++rm;
                    if (rm > 0) { textBuffer.erase(ls, rm); sel.anchor = sel.cursor = (sel.cursor > ls + rm) ? sel.cursor - rm : ls; dirty_ = true; }
                } else insertText(useTabs_ ? "\t" : std::string(tabSize_, ' '));
            }
            else if (sym == SDLK_LEFT) {
                if (ctrl) {
                    size_t pos = sel.cursor;
                    if (pos > 0) { --pos; while (pos > 0 && (textBuffer[pos] == ' ' || textBuffer[pos] == '\t')) --pos;
                        while (pos > 0 && (isalnum(textBuffer[pos-1]) || textBuffer[pos-1] == '_')) --pos; }
                    if (shift) sel.cursor = pos; else sel.anchor = sel.cursor = pos;
                } else { if (sel.cursor > 0) { if (shift) sel.cursor--; else sel.anchor = sel.cursor = sel.cursor - 1; } desiredCursorX_ = -1.f; }
            }
            else if (sym == SDLK_RIGHT) {
                if (ctrl) {
                    size_t pos = sel.cursor;
                    while (pos < textBuffer.size() && (textBuffer[pos] == ' ' || textBuffer[pos] == '\t')) ++pos;
                    while (pos < textBuffer.size() && (isalnum(textBuffer[pos]) || textBuffer[pos] == '_')) ++pos;
                    if (shift) sel.cursor = pos; else sel.anchor = sel.cursor = pos;
                } else { if (sel.cursor < textBuffer.size()) { if (shift) sel.cursor++; else sel.anchor = sel.cursor = sel.cursor + 1; } desiredCursorX_ = -1.f; }
            }
            else if (sym == SDLK_UP) {
                if (acActive_) { if (acSelected_ > 0) --acSelected_; }
                else { size_t cl = lineOfPos(sel.cursor); if (cl > 0) {
                    size_t ls = lineStartForLine(cl - 1), le = lineEnd(ls);
                    std::string_view lt(textBuffer.data() + ls, le - ls);
                    if (desiredCursorX_ < 0) desiredCursorX_ = fontAtlas().measureText(textBuffer.substr(lineStart(sel.cursor), sel.cursor - lineStart(sel.cursor)));
                    size_t col = 0; float adv = 0;
                    for (size_t i = 0; i < lt.size(); ) {
                        uint32_t cp = (uint8_t)lt[i]; int b = 1;
                        if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                        int cw = (cp == '\t') ? (int)fontAtlas().measureText(" ") * 2 : fontAtlas().getGlyph(cp).advance;
                        if (adv + cw / 2.f > desiredCursorX_) break; adv += cw; col += b; i += b;
                    }
                    size_t np = ls + col; if (shift) sel.cursor = np; else sel.anchor = sel.cursor = np;
                } }
            }
            else if (sym == SDLK_DOWN) {
                if (acActive_) { if (acSelected_ < (int)acItems_.size() - 1) ++acSelected_; }
                else { size_t cl = lineOfPos(sel.cursor); if (cl < totalLines() - 1) {
                    size_t nls = lineStartForLine(cl + 1), le = lineEnd(nls);
                    std::string_view lt(textBuffer.data() + nls, le - nls);
                    if (desiredCursorX_ < 0) desiredCursorX_ = fontAtlas().measureText(textBuffer.substr(lineStart(sel.cursor), sel.cursor - lineStart(sel.cursor)));
                    size_t col = 0; float adv = 0;
                    for (size_t i = 0; i < lt.size(); ) {
                        uint32_t cp = (uint8_t)lt[i]; int b = 1;
                        if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                        int cw = (cp == '\t') ? (int)fontAtlas().measureText(" ") * 2 : fontAtlas().getGlyph(cp).advance;
                        if (adv + cw / 2.f > desiredCursorX_) break; adv += cw; col += b; i += b;
                    }
                    size_t np = nls + col; if (shift) sel.cursor = np; else sel.anchor = sel.cursor = np;
                } }
            }
            else if (sym == SDLK_HOME) {
                if (ctrl) { if (shift) sel.cursor = 0; else sel.anchor = sel.cursor = 0; }
                else { size_t ls = lineStart(sel.cursor); if (shift) sel.cursor = ls; else sel.anchor = sel.cursor = ls; }
                desiredCursorX_ = -1.f;
            }
            else if (sym == SDLK_END) {
                if (ctrl) { if (shift) sel.cursor = textBuffer.size(); else sel.anchor = sel.cursor = textBuffer.size(); }
                else { size_t le = lineEnd(sel.cursor); if (shift) sel.cursor = le; else sel.anchor = sel.cursor = le; }
                desiredCursorX_ = -1.f;
            }
            else if (sym == SDLK_PAGEUP) {
                int w1, h1; SDL_GL_GetDrawableSize(window_, &w1, &h1);
                float vl = (h1 - titlebar_->height() - tabBarH_ - statusbar_->height()) / fontAtlas().lineHeight();
                size_t cl = lineOfPos(sel.cursor), tl = (cl > (size_t)vl) ? cl - (size_t)vl : 0;
                size_t p = lineStartForLine(tl); if (shift) sel.cursor = p; else sel.anchor = sel.cursor = p; desiredCursorX_ = -1.f;
            }
            else if (sym == SDLK_PAGEDOWN) {
                int w1, h1; SDL_GL_GetDrawableSize(window_, &w1, &h1);
                float vl = (h1 - titlebar_->height() - tabBarH_ - statusbar_->height()) / fontAtlas().lineHeight();
                size_t cl = lineOfPos(sel.cursor), tl = cl + (size_t)vl, tot = totalLines();
                if (tl >= tot) tl = tot - 1;
                size_t p = lineStartForLine(tl); if (shift) sel.cursor = p; else sel.anchor = sel.cursor = p; desiredCursorX_ = -1.f;
            }
            if (dirty_) syntaxDirty_ = true; indentsDirty_ = true;
            ensureCursorVisible();
        }
        else if (e.type == SDL_TEXTINPUT) { handleAutoPair(e.text.text); }
    }
}

void Application::update() {
    if (sidebarRefreshPending_.exchange(false) && !sidebarRoot_.empty()) {
        sidebarTree_.children.clear();
        buildSidebarNode(sidebarTree_);
    }
    if (syntaxDirty_) {
        syntax_->parse(textBuffer);
        syntaxDirty_ = false;
    }
}
void Application::updateTitle() {
    std::string t = openFile_; if (dirty_) t += "\xe2\x80\xa2"; t += " - Moreno Text";
    titlebar_->setTitle(t); SDL_SetWindowTitle(window_, t.c_str());
}

void Application::render() {
    GLRenderer::beginFrame();
    fontAtlas().resetMeasureCache();
    uint64_t fpsNow = SDL_GetPerformanceCounter();
    if (fpsLastCounter_ != 0) {
        double dtMs = (double)(fpsNow - fpsLastCounter_) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        fpsSmoothedMs_ = fpsSmoothedMs_ <= 0.f ? (float)dtMs : fpsSmoothedMs_ * 0.9f + (float)dtMs * 0.1f;
    }
    fpsLastCounter_ = fpsNow;
    solidVerts_.clear();
    auto addSolid = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        solidVerts_.insert(solidVerts_.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    auto flushSolid = [&] {
        if (solidVerts_.empty()) return;
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, solidVerts_.size()*sizeof(float), solidVerts_.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(solidVerts_.size()/8));
        glBindVertexArray(0); GLRenderer::setDrawMode(0); solidVerts_.clear();
    };
    updateTitle();
    int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
    float fww = (float)ww, fwh = (float)wh;
    float mmW = minimapVisible_ ? minimap_->width() : 0.f;
    float scrollbarW = minimapVisible_ ? 0.f : 10.f;
    float tbH = titlebar_->height() + tabBarH_;
    float sbH = statusbar_->height();
    float findPanelH = find_.active ? (find_.replaceActive ? 62.f : 30.f) : 0.f;
    float lineStep = fontAtlas().lineHeight();
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    size_t lineCount = totalLines();
    size_t currentLine = lineOfPos(selections_[0].cursor);
    size_t currentCol = colOfPos(selections_[0].cursor);
    size_t firstVisibleLine = (size_t)(scrollY_ / lineStep);
    size_t firstRenderLine = firstVisibleLine > 2 ? firstVisibleLine - 2 : 0;
    size_t lastRenderLine = lineCount > 0 ? std::min(lineCount - 1, (size_t)((scrollY_ + (fwh - sbH - findPanelH - tbH)) / lineStep) + 3) : 0;
    float firstVisibleY = textOriginY + firstVisibleLine * lineStep - scrollY_;
    float sidebarOffset = sidebarVisible_ ? sidebarWidth_ : 0.f;
    deferPopupDraw_ = titlebar_->isMenuOpen() || tabDropdownOpen_ || tabContextOpen_ || statusPopup_ != StatusPopup::None || commandPalette_.active || (acActive_ && !acItems_.empty());
    if (activeTab_ < tabs_.size()) {
        tabs_[activeTab_].fileName = openFile_.empty() ? "untitled" : openFile_;
        tabs_[activeTab_].filePath = openFilePath_;
        tabs_[activeTab_].dirty = dirty_;
    }

    drawTabBar(fontAtlas(), fww, titlebar_->height());
    drawSidebar(fontAtlas(), fwh, tbH, sbH);

    gutter_->draw(fontAtlas(), lineCount, currentLine, firstVisibleLine, sidebarOffset, firstVisibleY, lineStep, fwh - sbH, tbH);
    float gutterW = gutter_->width();
    float gutterRight = sidebarOffset + gutterW;
    float textX = gutterRight + 8.0f;
    float editorRight = fww - mmW - scrollbarW;
    float editorWidth = editorRight - textX;
    computeMaxLineWidth();
    float maxScrollX = (!wordWrap_ && editorWidth > 0.f) ? std::max(0.f, maxLineWidth_ - editorWidth) : 0.f;
    if (scrollX_ > maxScrollX) scrollX_ = maxScrollX;
    if (scrollX_ < 0.f || wordWrap_) scrollX_ = 0.f;
    bool horizontalScrollbarVisible = !wordWrap_ && maxScrollX > 0.f;
    float hScrollbarH = horizontalScrollbarVisible ? 10.f : 0.f;
    float editorBottom = fwh - sbH - findPanelH - hScrollbarH;
    float drawTextX = textX - scrollX_;

    glEnable(GL_SCISSOR_TEST);
    glScissor((int)gutterRight, (int)(sbH + hScrollbarH), ww - (int)gutterRight - (int)mmW, wh - (int)(tbH + sbH + hScrollbarH));

    {
        float cy = textOriginY + currentLine * lineStep - scrollY_;
        if (cy + lineStep >= tbH && cy <= editorBottom) {
            addSolid(gutterRight, cy, editorRight, cy + lineStep, .17f, .19f, .23f, 1.f);
        }
    }

    for (auto& s : selections_) {
        if (!s.hasSelection()) continue;
        size_t a = s.min(), b = s.max(), la = lineOfPos(a), lb = lineOfPos(b);
        if (lb < firstRenderLine || la > lastRenderLine) continue;
        la = std::max(la, firstRenderLine);
        lb = std::min(lb, lastRenderLine);
        for (size_t ln = la; ln <= lb; ++ln) {
            if (isFolded(ln)) continue;
            size_t ls = lineStartForLine(ln), le = lineEnd(ls);
            size_t ss = (ln == la) ? a : ls, se = (ln == lb) ? b : le;
            float sy = textOriginY + ln * lineStep - scrollY_;
            if (sy + lineStep < tbH || sy > editorBottom) continue;
            float sx = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, ss - ls));
            float ex = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, se - ls));
            addSolid(sx, sy, ex, sy + lineStep, accentColor_.r, accentColor_.g, accentColor_.b, 0.35f);
        }
    }

    if (find_.active && !find_.matches.empty()) {
        size_t qLen = find_.query.empty() ? 1 : find_.query.size();
        for (size_t mi = 0; mi < find_.matches.size(); ++mi) {
            size_t m = find_.matches[mi]; size_t ln = lineOfPos(m); size_t ls = lineStartForLine(ln);
            if (ln < firstRenderLine || ln > lastRenderLine) continue;
            float sy = textOriginY + ln * lineStep - scrollY_;
            if (sy + lineStep < tbH || sy > editorBottom) continue;
            float sx = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, m-ls));
            float ex = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, m+qLen-ls));
            bool cur = (mi == find_.currentMatch);
            addSolid(sx, sy, ex, sy + lineStep, cur?0.8f:0.6f, cur?0.7f:0.6f, 0.2f, 0.5f);
        }
    }
    // text lines with syntax highlighting
    // indent guides + fold ... + whitespace dots
    float spaceWidth = fontAtlas().measureText(" ");
    float y = textOriginY - scrollY_;
    size_t lineIdx = 0, lStart = 0;
    // build indent guide data: track per-line indent
    computeLineIndents();
    // draw indent guides (vertical 1px lines at each indent level)
    {
        float gy = textOriginY + firstRenderLine * lineStep - scrollY_;
        for (size_t ln = firstRenderLine; ln <= lastRenderLine && ln < totalLines(); ++ln) {
            if (isFolded(ln)) continue;
            if (gy + lineStep < tbH) { gy += lineStep; continue; }
            if (gy > fwh) break;
            int indent = (ln < lineIndents_.size()) ? lineIndents_[ln] : 0;
            int cursorIndent = (currentLine < lineIndents_.size()) ? lineIndents_[currentLine] : 0;
            int activeLvl = cursorIndent > 0 ? ((cursorIndent - 1) / tabSize_) * tabSize_ : 0;
            for (int lvl = tabSize_; lvl <= indent; lvl += tabSize_) {
                float gx = drawTextX + lvl * spaceWidth;
                bool active = activeLvl > 0 && lvl == activeLvl;
                if (active) addSolid(gx, gy, gx + 1, gy + lineStep, 0.420f, 0.447f, 0.502f, 0.75f);
                else addSolid(gx, gy, gx + 1, gy + lineStep, 0.294f, 0.314f, 0.345f, 0.55f);
            }
            gy += lineStep;
        }
    }

    flushSolid();
    // text lines
    y = textOriginY + firstRenderLine * lineStep - scrollY_;
    lineIdx = firstRenderLine; lStart = lineStartForLine(firstRenderLine);
    while (lStart <= textBuffer.size()) {
        size_t lEnd = textBuffer.find('\n', lStart);
        if (lEnd == std::string::npos) lEnd = textBuffer.size();
        if (!isFolded(lineIdx) && y + lineStep > tbH && y < editorBottom) {
            std::string_view line(textBuffer.data() + lStart, lEnd - lStart);
            auto tokens = syntax_->highlightLine(line, lStart);
            if (tokens.empty()) {
                auto& c = syntax_->scopeColor(0);
                fontAtlas().drawText(line, drawTextX, y, c.r, c.g, c.b, 1.0f);
            } else {
                float cx = drawTextX;
                size_t prevEnd = 0;
                for (auto& tok : tokens) {
                    if (tok.start > lStart + prevEnd) {
                        auto& c = syntax_->scopeColor(0);
                        std::string_view gap(textBuffer.data() + lStart + prevEnd, tok.start - lStart - prevEnd);
                        fontAtlas().drawText(gap, cx, y, c.r, c.g, c.b, 1.0f);
                        cx += fontAtlas().measureText(gap);
                    }
                    auto& c = syntax_->scopeColor(tok.scope);
                    std::string_view tokText(textBuffer.data() + tok.start, tok.length);
                    fontAtlas().drawText(tokText, cx, y, c.r, c.g, c.b, 1.0f);
                    cx += fontAtlas().measureText(tokText);
                    prevEnd = (tok.start - lStart) + tok.length;
                }
                if (prevEnd < lEnd - lStart) {
                    auto& c = syntax_->scopeColor(0);
                    std::string_view rest(textBuffer.data() + lStart + prevEnd, lEnd - lStart - prevEnd);
                    fontAtlas().drawText(rest, cx, y, c.r, c.g, c.b, 1.0f);
                }
            }
            if (isFoldStart(lineIdx)) {
                float ellipsisX = drawTextX + fontAtlas().measureText(line) + fontAtlas().measureText(" ");
                fontAtlas().drawText("...", ellipsisX, y, 0.55f, 0.55f, 0.60f, 1.f);
            }
        }
        y += isFolded(lineIdx) ? 0 : lineStep;
        ++lineIdx; lStart = lEnd + 1;
        if (lineIdx > lastRenderLine || y > fwh) break;
    }

    {
        for (auto& s : selections_) {
            if (!s.hasSelection()) continue;
            size_t sa = s.min(), sb = s.max();
            size_t la = lineOfPos(sa), lb = lineOfPos(sb);
            if (lb < firstRenderLine || la > lastRenderLine) continue;
            la = std::max(la, firstRenderLine);
            lb = std::min(lb, lastRenderLine);
            for (size_t ln = la; ln <= lb; ++ln) {
                size_t ls = lineStartForLine(ln), le = lineEnd(ls);
                size_t start = std::max(sa, ls);
                size_t end = std::min(sb, le);
                float wy = textOriginY + ln * lineStep - scrollY_;
                if (wy + lineStep < tbH || wy > editorBottom) continue;
                for (size_t pos = start; pos < end; ++pos) {
                char c = textBuffer[pos];
                if (c != ' ' && c != '\t') continue;
                float wx = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, pos - ls));
                if (c == ' ') {
                    float cx = wx + spaceWidth / 2.f, cy = wy + lineStep / 2.f, r = 1.2f;
                    addSolid(cx-r, cy-r, cx+r, cy+r, 0.45f, 0.45f, 0.5f, 0.6f);
                } else {
                    float cx = wx + 2.f, cy = wy + lineStep * 0.3f;
                    addSolid(cx, cy, cx+5, cy+lineStep*0.2f, 0.45f, 0.45f, 0.5f, 0.6f);
                    addSolid(cx, cy+lineStep*0.4f, cx+5, cy+lineStep*0.2f, 0.45f, 0.45f, 0.5f, 0.6f);
                }
                }
            }
        }
    }

    {
        std::vector<float> fv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            fv.insert(fv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        auto tri = [&](float x0,float y0,float x1,float y1,float x2,float y2,float r,float g,float b,float a) {
            fv.insert(fv.end(),{x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x2,y2,0,0,r,g,b,a});
        };
        float gy = textOriginY + firstRenderLine * lineStep - scrollY_;
        for (size_t ln = firstRenderLine; ln <= lastRenderLine && ln < totalLines(); ++ln) {
            if (isFolded(ln)) continue;
            if (gy + lineStep < tbH || gy > editorBottom) { gy += lineStep; continue; }
            bool foldable = isFoldableLine(ln) || isFoldStart(ln);
            if (foldable) {
                float tx = 6.f, ty = gy + lineStep / 2.f - 4.f;
                if (isFoldStart(ln)) tri(tx, ty, tx, ty + 8.f, tx + 7.f, ty + 4.f, 0.72f, 0.72f, 0.76f, 1.f);
                else tri(tx, ty + 2.f, tx + 8.f, ty + 2.f, tx + 4.f, ty + 8.f, 0.55f, 0.55f, 0.60f, 1.f);
            }
            if (bookmarks_.count((int)ln)) {
                float cx = gutter_->width() - 14.f, cy = gy + lineStep / 2.f;
                float r = 3.f;
                tri(cx - r, cy - r, cx + r, cy - r, cx, cy + r, 0.75f, 0.65f, 0.25f, 1.f);
                tri(cx - r, cy + r, cx + r, cy + r, cx, cy - r, 0.75f, 0.65f, 0.25f, 1.f);
                tri(cx - r, cy - r, cx, cy + r, cx - r, cy + r, 0.75f, 0.65f, 0.25f, 1.f);
                tri(cx + r, cy - r, cx, cy + r, cx + r, cy + r, 0.75f, 0.65f, 0.25f, 1.f);
            }
            gy += lineStep;
        }
        if (!fv.empty()) { GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, fv.size()*sizeof(float), fv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(fv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0); }
    }

    uint32_t cursorNowMs = SDL_GetTicks();
    bool cursorVisible = (cursorNowMs - cursorLastInputTicks_ < 1000u) || (((cursorNowMs / 500u) % 2u) == 0u);
    if (cursorVisible) for (auto& s : selections_) {
        size_t cl = lineOfPos(s.cursor), ls = lineStartForLine(cl);
        float cx = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, s.cursor-ls));
        float cy = textOriginY + cl * lineStep - scrollY_;
        if (cy + lineStep < tbH || cy > editorBottom) continue;
        float ct = cy, cb = cy + fontAtlas().ascent() - fontAtlas().descent();
        std::vector<float> cv = {cx,ct,0,0,accentColor_.r,accentColor_.g,accentColor_.b,1.f, cx,cb,0,0,accentColor_.r,accentColor_.g,accentColor_.b,1.f, cx+2,cb,0,0,accentColor_.r,accentColor_.g,accentColor_.b,1.f, cx,ct,0,0,accentColor_.r,accentColor_.g,accentColor_.b,1.f, cx+2,cb,0,0,accentColor_.r,accentColor_.g,accentColor_.b,1.f, cx+2,ct,0,0,accentColor_.r,accentColor_.g,accentColor_.b,1.f};
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, cv.size()*sizeof(float), cv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, fontAtlas().atlasTexture()); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0);
    }
    // bracket matching
    {
        auto drawBracketBox = [&](size_t pos, float r, float g, float b) {
            if (pos >= textBuffer.size()) return;
            size_t ln = lineOfPos(pos), ls = lineStartForLine(ln);
            float bx = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, pos-ls));
            float by = textOriginY + ln * lineStep - scrollY_;
            if (by + lineStep < tbH || by > editorBottom) return;
            float bw = fontAtlas().getGlyph((uint8_t)textBuffer[pos]).advance;
            float ct = by, cb = by + fontAtlas().ascent() - fontAtlas().descent();
            float t = 1.f;
            addSolid(bx-t, ct-t, bx+bw+t, ct, r, g, b, 0.7f);
            addSolid(bx-t, cb, bx+bw+t, cb+t, r, g, b, 0.7f);
            addSolid(bx-t, ct, bx, cb, r, g, b, 0.7f);
            addSolid(bx+bw, ct, bx+bw+t, cb, r, g, b, 0.7f);
        };
        auto findMatch = [&](size_t pos) -> size_t {
            if (pos >= textBuffer.size()) return (size_t)-1;
            char c = textBuffer[pos];
            char openCh = 0, closeCh = 0; bool forward = true;
            if (c=='(') { openCh='('; closeCh=')'; }
            else if (c==')') { openCh='('; closeCh=')'; forward=false; }
            else if (c=='[') { openCh='['; closeCh=']'; }
            else if (c==']') { openCh='['; closeCh=']'; forward=false; }
            else if (c=='{') { openCh='{'; closeCh='}'; }
            else if (c=='}') { openCh='{'; closeCh='}'; forward=false; }
            else return (size_t)-1;
            int depth = 1;
            if (forward) { for (size_t p = pos+1; p < textBuffer.size(); ++p) { if (textBuffer[p]==openCh) ++depth; else if (textBuffer[p]==closeCh) { if (--depth==0) return p; } } }
            else { for (size_t p = pos; p-- > 0; ) { if (textBuffer[p]==closeCh) ++depth; else if (textBuffer[p]==openCh) { if (--depth==0) return p; } } }
            return (size_t)-1;
        };
        size_t cur = selections_[0].cursor;
        for (int off : {0, (cur > 0 ? -1 : 0)}) {
            size_t pos = (off == -1) ? cur - 1 : cur;
            if (pos >= textBuffer.size()) continue;
            char c = textBuffer[pos];
            if (c!='(' && c!=')' && c!='[' && c!=']' && c!='{' && c!='}') continue;
            size_t match = findMatch(pos);
            if (match != (size_t)-1) { drawBracketBox(pos, 0.325f, 0.545f, 1.f); drawBracketBox(match, 0.325f, 0.545f, 1.f); }
            else { drawBracketBox(pos, 0.878f, 0.424f, 0.459f); }
            break;
        }
    }
    glDisable(GL_SCISSOR_TEST);
    // scrollbar
    if (!minimapVisible_) {
        float sbX = fww - mmW - scrollbarW;
        float contentH = totalLines() * lineStep;
        float scrollPad = scrollPastEnd_ ? lineStep * 5.f : 0.f;
        float viewH = fwh - tbH - sbH;
        float thumbFrac = (contentH + scrollPad > 0) ? viewH / (contentH + scrollPad) : 1.f;
        if (thumbFrac > 1.f) thumbFrac = 1.f;
        float thumbH = viewH * thumbFrac;
        if (thumbH < 20.f) thumbH = 20.f;
        float maxScroll = contentH + scrollPad - viewH;
        if (maxScroll < 0.f) maxScroll = 0.f;
        float thumbY = (maxScroll > 0) ? tbH + (scrollY_ / maxScroll) * (viewH - thumbH) : tbH;
        std::vector<float> sv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            sv.insert(sv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        float thumbW = 4.f;
        float thumbX = sbX + (scrollbarW - thumbW) / 2.f;
        float c = scrollbarDragging_ ? 0.416f : (scrollbarHovered_ ? 0.353f : 0.290f);
        ar(thumbX, thumbY, thumbX + thumbW, thumbY + thumbH, c, c, c, 1.f);
        if (!sv.empty()) { GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, sv.size()*sizeof(float), sv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(sv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0); }
    }
    if (horizontalScrollbarVisible) {
        float trackY = fwh - sbH - findPanelH - hScrollbarH;
        float regionW = editorWidth;
        float thumbW = std::max(20.f, regionW * (regionW / maxLineWidth_));
        if (thumbW > regionW) thumbW = regionW;
        float thumbX = textX + (maxScrollX > 0.f ? (scrollX_ / maxScrollX) * (regionW - thumbW) : 0.f);
        float c = horizontalScrollbarDragging_ ? 0.416f : (horizontalScrollbarHovered_ ? 0.353f : 0.290f);
        addSolid(thumbX, trackY + 3.f, thumbX + thumbW, trackY + 9.f, c, c, c, 1.f);
        flushSolid();
    }
    // minimap
    if (minimapVisible_) {
        bool mmOver = mouseX_ >= (int)(fww - mmW) && mouseY_ >= (int)tbH && mouseY_ < (int)(fwh - sbH - findPanelH);
        minimap_->setMouseOver(mmOver);
        minimap_->updateHoverFade(1.f/60.f);
        minimap_->draw(fontAtlas(), *syntax_, textBuffer, fww - mmW, textOriginY, fwh - findPanelH, tbH, gutterW, lineStep, scrollY_, mmOver);
    }
    // status bar
    std::string branch;
    { std::lock_guard<std::mutex> lock(gitBranchMutex_); branch = gitBranch_; }
    statusbar_->appendSolidRects(fontAtlas(), solidVerts_, fww, fwh, currentLine, currentCol, branch);
    flushSolid();
    statusbar_->drawText(fontAtlas(), fww, fwh, currentLine, currentCol, syntax_->languageName(), useTabs_, tabSize_, branch);
    // find bar
    if (find_.active) {
        float rowH = 30.f, barH = find_.replaceActive ? rowH * 2.f + 2.f : rowH;
        float barY = fwh - sbH - barH;
        float barW = fww;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(0, barY, barW, barY + barH, 0.16f, 0.16f, 0.19f, 1.f);
        ar(0, barY, barW, barY + 1, 0.30f, 0.30f, 0.35f, 1.f);
        float fieldX = 108.f, fieldH = 22.f;
        float rightButtonsW = find_.replaceActive ? 286.f : 164.f;
        float fieldW = std::max(120.f, barW - fieldX - rightButtonsW);
        float fy = barY + (rowH - fieldH) / 2.f;
        ar(fieldX, fy, fieldX + fieldW, fy + fieldH, 0.12f, 0.12f, 0.14f, 1.f);
        float btnX = 8.f;
        auto toggleBtn = [&](const char* label, bool active, float x) {
            ar(x, fy, x + 28.f, fy + fieldH, active ? 0.25f : 0.14f, active ? 0.28f : 0.14f, active ? 0.35f : 0.16f, 1.f);
        };
        toggleBtn("/Re", find_.regex, btnX); btnX += 32.f;
        toggleBtn("Aa", find_.caseSensitive, btnX); btnX += 32.f;
        toggleBtn("\\b", find_.wholeWord, btnX); btnX += 32.f;
        float prevButtonX = fieldX + fieldW + 8.f;
        float nextButtonX = prevButtonX + 62.f;
        float closeButtonX = barW - 30.f;
        ar(prevButtonX, fy, prevButtonX + 56.f, fy + fieldH, 0.18f, 0.18f, 0.21f, 1.f);
        ar(nextButtonX, fy, nextButtonX + 56.f, fy + fieldH, 0.18f, 0.18f, 0.21f, 1.f);
        ar(closeButtonX, fy, closeButtonX + 22.f, fy + fieldH, 0.18f, 0.18f, 0.21f, 1.f);
        float replaceFieldY = 0.f;
        float replaceButtonX = 0.f;
        float replaceAllButtonX = 0.f;
        if (find_.replaceActive) {
            float ry = barY + rowH + 2.f;
            float rfy = ry + (rowH - fieldH) / 2.f;
            replaceFieldY = rfy;
            ar(8.f, rfy, 36.f, rfy + fieldH, 0.14f, 0.14f, 0.16f, 1.f);
            ar(40.f, rfy, 68.f, rfy + fieldH, 0.14f, 0.14f, 0.16f, 1.f);
            ar(fieldX, rfy, fieldX + fieldW, rfy + fieldH, 0.12f, 0.12f, 0.14f, 1.f);
            replaceButtonX = fieldX + fieldW + 8.f;
            replaceAllButtonX = replaceButtonX + 82.f;
            ar(replaceButtonX, rfy, replaceButtonX + 76.f, rfy + fieldH, 0.18f, 0.18f, 0.21f, 1.f);
            ar(replaceAllButtonX, rfy, replaceAllButtonX + 96.f, rfy + fieldH, 0.18f, 0.18f, 0.21f, 1.f);
        }
        flushSolid();
        std::string findText = find_.query + (findFocus_ == 0 ? "|" : "");
        fontAtlas().drawText(findText, fieldX + 6.f, fy + 4.f, 0.85f, 0.85f, 0.88f, 1.f);
        if (!find_.matches.empty()) {
            std::string mc = std::to_string(find_.matches.size()) + " matches";
            float mcW = fontAtlas().measureText(mc);
            fontAtlas().drawText(mc, fieldX + fieldW - mcW - 8.f, fy + 4.f, 0.5f, 0.5f, 0.55f, 1.f);
        } else if (!find_.query.empty()) {
            float mw = fontAtlas().measureText("No matches");
            fontAtlas().drawText("No matches", fieldX + fieldW - mw - 8.f, fy + 4.f, 0.6f, 0.3f, 0.3f, 1.f);
        }
        fontAtlas().drawText("/Re", 12.f, fy + 4.f, find_.regex ? 0.95f : 0.45f, find_.regex ? 0.85f : 0.45f, find_.regex ? 0.95f : 0.5f, 1.f);
        fontAtlas().drawText("Aa", 44.f, fy + 4.f, find_.caseSensitive ? 0.95f : 0.45f, find_.caseSensitive ? 0.85f : 0.45f, find_.caseSensitive ? 0.95f : 0.5f, 1.f);
        fontAtlas().drawText("\\b", 76.f, fy + 4.f, find_.wholeWord ? 0.95f : 0.45f, find_.wholeWord ? 0.85f : 0.45f, find_.wholeWord ? 0.95f : 0.5f, 1.f);
        fontAtlas().drawText("Prev", prevButtonX + 9.f, fy + 4.f, 0.65f, 0.70f, 0.65f, 1.f);
        fontAtlas().drawText("Next", nextButtonX + 9.f, fy + 4.f, 0.70f, 0.75f, 0.70f, 1.f);
        fontAtlas().drawText("\xc3\x97", closeButtonX + 7.f, fy + 3.f, 0.6f, 0.6f, 0.6f, 1.f);
        if (find_.replaceActive) {
            std::string repText = find_.replace + (findFocus_ == 1 ? "|" : "");
            fontAtlas().drawText("^", 17.f, replaceFieldY + 4.f, 0.45f, 0.45f, 0.5f, 1.f);
            fontAtlas().drawText("<>", 45.f, replaceFieldY + 4.f, 0.45f, 0.45f, 0.5f, 1.f);
            fontAtlas().drawText(repText, fieldX + 6.f, replaceFieldY + 4.f, 0.85f, 0.85f, 0.88f, 1.f);
            fontAtlas().drawText("Replace", replaceButtonX + 8.f, replaceFieldY + 4.f, 0.7f, 0.75f, 0.7f, 1.f);
            fontAtlas().drawText("All", replaceAllButtonX + 34.f, replaceFieldY + 4.f, 0.7f, 0.75f, 0.7f, 1.f);
        }
    }
    // goto overlay
    if (goto_.active) {
        float ow = 400.f, oh = 200.f, ox = (fww - ow) / 2.f, oy = tbH + 20.f;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(ox, oy, ox + ow, oy + oh, 0.15f, 0.15f, 0.18f, 0.97f);
        ar(ox, oy, ox + ow, oy + 28, 0.12f, 0.12f, 0.14f, 1.f);
        // selected item highlight
        if (goto_.selected >= 0 && goto_.selected < (int)goto_.items.size())
            ar(ox, oy + 28 + goto_.selected * 22, ox + ow, oy + 28 + (goto_.selected + 1) * 22, 0.22f, 0.28f, 0.42f, 1.f);
        flushSolid();
        fontAtlas().drawText("Goto: " + goto_.query + "|", ox + 8, oy + 6, 0.8f, 0.8f, 0.8f, 1.f);
        const char* modeLabel = gotoMode_ == Symbols ? "Symbols" : gotoMode_ == Lines ? "Go to Line" : gotoMode_ == Words ? "Words" : "Files";
        float mlW = fontAtlas().measureText(modeLabel);
        fontAtlas().drawText(modeLabel, ox + ow - mlW - 12, oy + 6, 0.45f, 0.45f, 0.5f, 1.f);
        for (int i = 0; i < (int)goto_.items.size() && i < 10; ++i) {
            float ib = (i == goto_.selected) ? 1.f : 0.7f;
            fontAtlas().drawText(goto_.items[i], ox + 8, oy + 30 + i * 22, ib, ib, ib, 1.f);
            if (i < (int)goto_.subtexts.size() && !goto_.subtexts[i].empty())
                fontAtlas().drawText(goto_.subtexts[i], ox + 200, oy + 30 + i * 22, 0.4f, 0.4f, 0.45f, 1.f);
        }
    }
    if (commandPalette_.active && !deferPopupDraw_) {
        float ow = 560.f, oh = 286.f, ox = (fww - ow) / 2.f, oy = tbH + 36.f;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(ox, oy, ox + ow, oy + oh, 0.15f, 0.15f, 0.18f, 0.98f);
        ar(ox, oy, ox + ow, oy + 32.f, 0.12f, 0.12f, 0.14f, 1.f);
        if (commandPalette_.selected >= 0 && commandPalette_.selected < (int)commandPalette_.results.size())
            ar(ox + 2.f, oy + 34.f + commandPalette_.selected * 24.f, ox + ow - 2.f, oy + 34.f + (commandPalette_.selected + 1) * 24.f, 0.25f, 0.30f, 0.45f, 1.f);
        flushSolid();
        fontAtlas().drawText("Command Palette: " + commandPalette_.query + "|", ox + 10.f, oy + 8.f, 0.84f, 0.84f, 0.86f, 1.f);
        int visible = std::min(10, (int)commandPalette_.results.size());
        for (int row = 0; row < visible; ++row) {
            int idx = commandPalette_.results[row];
            float b = row == commandPalette_.selected ? 0.98f : 0.72f;
            fontAtlas().drawText(paletteCommands[idx].label, ox + 10.f, oy + 38.f + row * 24.f, b, b, b + 0.03f, 1.f);
            if (paletteCommands[idx].shortcut[0]) {
                float sw = fontAtlas().measureText(paletteCommands[idx].shortcut);
                fontAtlas().drawText(paletteCommands[idx].shortcut, ox + ow - sw - 14.f, oy + 38.f + row * 24.f, 0.48f, 0.48f, 0.52f, 1.f);
            }
        }
    }
    // autocomplete popup
    if (acActive_ && !acItems_.empty() && !deferPopupDraw_) {
        size_t cur = selections_[0].cursor, ls = lineStart(cur);
        float cx = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, cur-ls));
        size_t cl = lineOfPos(cur);
        float cy = textOriginY + cl * lineStep - scrollY_ + fontAtlas().ascent() - fontAtlas().descent() + 4.f;
        float acW = 220.f, itemH = 22.f, acH = acItems_.size() * itemH + 4.f;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(cx, cy, cx + acW, cy + acH, 0.17f, 0.19f, 0.23f, 0.98f);
        if (acSelected_ >= 0 && acSelected_ < (int)acItems_.size())
            ar(cx + 2, cy + 2 + acSelected_ * itemH, cx + acW - 2, cy + 2 + (acSelected_ + 1) * itemH, 0.24f, 0.27f, 0.32f, 1.f);
        flushSolid();
        for (int i = 0; i < (int)acItems_.size(); ++i) {
            float b = (i == acSelected_) ? 1.f : 0.7f;
            fontAtlas().drawText(acItems_[i], cx + 8, cy + 4 + i * itemH, b, b, b, 1.f);
        }
    }

    if (statusPopup_ != StatusPopup::None && !deferPopupDraw_) {
        int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
        // compute popup width from content
        float maxTextW = 100.f;
        if (statusPopup_ == StatusPopup::Indent) {
            const char* indentItems[] = {"Indent Using Spaces","Indent Using Tabs","Tab Width: 2","Tab Width: 4","Tab Width: 8","Convert Indentation to Spaces","Convert Indentation to Tabs","Detect Indentation"};
            for (auto* item : indentItems) { float w=fontAtlas().measureText(item)+48.f; if(w>maxTextW) maxTextW=w; }
            float w=fontAtlas().measureText("Convert Indentation to Spaces")+32.f; if(w>maxTextW) maxTextW=w;
        } else {
            for (int i = 0; i < syntaxLangCount; ++i) { float w=fontAtlas().measureText(syntaxLanguages[i])+32.f; if(w>maxTextW) maxTextW=w; }
        }
        float popW = maxTextW, popH = itemCount * 24.f + 4.f;
        std::vector<float> pv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            pv.insert(pv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        ar(popupX_, popupY_, popupX_ + popW, popupY_ + popH, 0.18f, 0.18f, 0.21f, 0.98f);
        ar(popupX_, popupY_, popupX_ + popW, popupY_ + 1, 0.3f, 0.3f, 0.35f, 1.f);
        if (popupSelected_ >= 0 && popupSelected_ < itemCount)
            ar(popupX_ + 2, popupY_ + 2 + popupSelected_ * 24.f, popupX_ + popW - 2, popupY_ + 2 + (popupSelected_ + 1) * 24.f, 0.25f, 0.30f, 0.45f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, pv.size()*sizeof(float), pv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(pv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)popupX_, wh - (int)(popupY_ + popH + 2), (int)popW + 2, (int)popH + 2);
        if (statusPopup_ == StatusPopup::Indent) {
            const char* indentItems[] = {"Indent Using Spaces","Indent Using Tabs","Tab Width: 2","Tab Width: 4","Tab Width: 8","Convert Indentation to Spaces","Convert Indentation to Tabs","Detect Indentation"};
            for (int i = 0; i < itemCount; ++i) {
                bool active = (i == 0 && !useTabs_) || (i == 1 && useTabs_) || (i == 2 && tabSize_ == 2) || (i == 3 && tabSize_ == 4) || (i == 4 && tabSize_ == 8);
                if (active) fontAtlas().drawText("\xe2\x80\xa2", popupX_ + 8, popupY_ + 6 + i * 24.f, 0.9f, 0.9f, 1.f, 1.f);
                float b = active ? 0.9f : 0.7f;
                fontAtlas().drawText(indentItems[i], popupX_ + 24, popupY_ + 6 + i * 24.f, b, b, active ? 1.f : 0.75f, 1.f);
            }
        } else {
            for (int row = 0; row < itemCount; ++row) {
                int i = popupScroll_ + row;
                if (i >= syntaxLangCount) break;
                float b = (syntax_->languageName() == syntaxLanguages[i]) ? 1.f : 0.7f;
                if (syntax_->languageName() == syntaxLanguages[i])
                    fontAtlas().drawText("\xe2\x80\xa2", popupX_ + 8, popupY_ + 6 + row * 24.f, 0.9f, 0.9f, 1.f, 1.f);
                fontAtlas().drawText(syntaxLanguages[i], popupX_ + 24, popupY_ + 6 + row * 24.f, b, b, b, 1.f);
            }
        }
        glDisable(GL_SCISSOR_TEST);
    }

    if (titlebar_->isMenuOpen() && deferPopupDraw_) titlebar_->deferMenuDraw();
    titlebar_->appendSolidRects(solidVerts_);
    flushSolid();
    titlebar_->drawForeground(fontAtlas());
    if (closeConfirmOpen_) {
        float mw = 420.f, mh = 118.f, mx = (fww - mw) / 2.f, my = (fwh - mh) / 2.f;
        std::vector<float> mv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            mv.insert(mv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        ar(mx, my, mx + mw, my + mh, 0.17f, 0.17f, 0.20f, 0.98f);
        ar(mx, my, mx + mw, my + 1.f, 0.35f, 0.35f, 0.40f, 1.f);
        float by = my + 76.f, bw = 96.f, bh = 26.f;
        ar(mx + 72.f, by, mx + 72.f + bw, by + bh, 0.24f, 0.28f, 0.34f, 1.f);
        ar(mx + 182.f, by, mx + 182.f + bw, by + bh, 0.24f, 0.28f, 0.34f, 1.f);
        ar(mx + 292.f, by, mx + 292.f + bw, by + bh, 0.24f, 0.28f, 0.34f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, mv.size()*sizeof(float), mv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(mv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        std::string name = closeConfirmIndex_ < tabs_.size() ? tabs_[closeConfirmIndex_].fileName : openFile_;
        fontAtlas().drawText("Save changes to " + name + " before closing?", mx + 22.f, my + 28.f, 0.86f, 0.86f, 0.88f, 1.f);
        fontAtlas().drawText("Save", mx + 103.f, by + 6.f, 0.82f, 0.82f, 0.86f, 1.f);
        fontAtlas().drawText("Don't Save", mx + 198.f, by + 6.f, 0.82f, 0.82f, 0.86f, 1.f);
        fontAtlas().drawText("Cancel", mx + 315.f, by + 6.f, 0.82f, 0.82f, 0.86f, 1.f);
    }
    if (debugFpsVisible_) {
        float ms = fpsSmoothedMs_ > 0.f ? fpsSmoothedMs_ : 0.f;
        float fps = ms > 0.f ? 1000.f / ms : 0.f;
        char fpsBuf[64];
        snprintf(fpsBuf, sizeof(fpsBuf), "FPS %.0f  %.1f ms", fps, ms);
        float pad = 6.f;
        float w = fontAtlas().measureText(fpsBuf) + pad * 2.f;
        float x = fww - w - 10.f;
        float fpsY = titlebar_->height() + tabBarH_ + 8.f;
        addSolid(x, fpsY, x + w, fpsY + 22.f, 0.05f, 0.05f, 0.06f, 0.82f);
        flushSolid();
        fontAtlas().drawText(fpsBuf, x + pad, fpsY + 4.f, 0.75f, 0.95f, 0.75f, 1.f);
    }
    GLRenderer::endFrame();
    SDL_GL_SwapWindow(window_);
    // render overflow popups to separate window
    bool hasPopup = false;
    float mainX = 0.f, mainY = 0.f, mainW = 0.f, mainH = 0.f;
    enum class PopupKind { None, Menu, TabDropdown, TabContext, Status, CommandPalette, Autocomplete } popupKind = PopupKind::None;
    if (titlebar_->isMenuOpen()) {
        titlebar_->getMenuPopupBounds(fontAtlas(), mainX, mainY, mainW, mainH);
        popupKind = PopupKind::Menu; hasPopup = true;
    } else if (tabDropdownOpen_) {
        mainX = tabChevronX_; mainY = titlebar_->height() + tabBarH_;
        mainW = 260.f; mainH = static_cast<float>(tabs_.size()) * 24.f + 4.f;
        popupKind = PopupKind::TabDropdown; hasPopup = true;
    } else if (tabContextOpen_) {
        mainX = tabContextX_; mainY = tabContextY_;
        int itemCount = tabContextIndex_ != activeTab_ ? 13 : 12;
        mainW = 260.f; mainH = 4.f + itemCount * 24.f;
        popupKind = PopupKind::TabContext; hasPopup = true;
    } else if (statusPopup_ != StatusPopup::None) {
        int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
        float maxTextW = 100.f;
        if (statusPopup_ == StatusPopup::Indent) {
            const char* indentItems[] = {"Indent Using Spaces","Indent Using Tabs","Tab Width: 2","Tab Width: 4","Tab Width: 8","Convert Indentation to Spaces","Convert Indentation to Tabs","Detect Indentation"};
            for (auto* item : indentItems) maxTextW = std::max(maxTextW, fontAtlas().measureText(item) + 48.f);
        } else {
            for (int i = 0; i < syntaxLangCount; ++i) maxTextW = std::max(maxTextW, fontAtlas().measureText(syntaxLanguages[i]) + 32.f);
        }
        mainX = popupX_; mainY = popupY_; mainW = maxTextW; mainH = itemCount * 24.f + 4.f;
        popupKind = PopupKind::Status; hasPopup = true;
    } else if (commandPalette_.active) {
        mainW = 560.f; mainH = 330.f;
        mainX = (fww - mainW) / 2.f; mainY = tbH + 36.f;
        popupKind = PopupKind::CommandPalette; hasPopup = true;
    } else if (acActive_ && !acItems_.empty()) {
        size_t cur = selections_[0].cursor, ls = lineStart(cur);
        size_t cl = lineOfPos(cur);
        mainX = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, cur-ls));
        mainY = textOriginY + cl * lineStep - scrollY_ + fontAtlas().ascent() - fontAtlas().descent() + 4.f;
        mainW = 220.f; mainH = static_cast<float>(acItems_.size()) * 22.f + 4.f;
        popupKind = PopupKind::Autocomplete; hasPopup = true;
    }
    if (hasPopup && mainW > 1.f && mainH > 1.f) {
        int winX = 0, winY = 0; SDL_GetWindowPosition(window_, &winX, &winY);
        popupMainX_ = mainX; popupMainY_ = mainY; popupMainW_ = mainW; popupMainH_ = mainH;
        renderPopupToWindow(winX + static_cast<int>(mainX), winY + static_cast<int>(mainY), static_cast<int>(mainW), static_cast<int>(mainH));
        if (popupKind == PopupKind::Menu) shapePopupWindowForMenu();
        else clearPopupWindowShape();
        auto drawRect = [](std::vector<float>& verts, float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            verts.insert(verts.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        auto flush = [](std::vector<float>& verts) {
            if (verts.empty()) return;
            GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
            glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
            glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size()/8));
            glBindVertexArray(0); GLRenderer::setDrawMode(0); verts.clear();
        };
        if (popupKind == PopupKind::Menu) {
            titlebar_->drawMenuPopup(fontAtlas(), mainX, mainY);
        } else if (popupKind == PopupKind::TabDropdown) {
            std::vector<float> pv;
            float itemH = 24.f;
            drawRect(pv, 0, 0, mainW, mainH, 0.17f, 0.17f, 0.20f, 0.98f);
            if (tabDropdownHover_ >= 0 && tabDropdownHover_ < (int)tabs_.size())
                drawRect(pv, 2, 2 + tabDropdownHover_ * itemH, mainW - 2, 2 + (tabDropdownHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
            flush(pv);
            for (size_t i = 0; i < tabs_.size(); ++i) {
                std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
                if (tabs_[i].dirty) label += "\xe2\x80\xa2";
                float b = i == activeTab_ ? 0.95f : 0.70f;
                fontAtlas().drawText(label, 10.f, 6.f + i * itemH, b, b, b + 0.03f, 1.f);
            }
        } else if (popupKind == PopupKind::TabContext) {
            const char* items[] = {"Close Tab","Close Other Tabs","Close Tabs to the Right","Close Unmodified Tabs","Close Unmodified Tabs to the Right","Close Tabs With Deleted Files","","Split View","","New File","Open File    Ctrl+O","","Diff With Current Tab..."};
            std::vector<float> cv; float itemH = 24.f;
            int itemCount = tabContextIndex_ != activeTab_ ? 13 : 12;
            drawRect(cv, 0, 0, mainW, mainH, 0.17f, 0.17f, 0.20f, 0.98f);
            for (int i = 0; i < itemCount; ++i) if (!items[i][0]) drawRect(cv, 8.f, 13.f + i * itemH, mainW - 8.f, 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f);
            if (tabContextHover_ >= 0 && tabContextHover_ < itemCount && items[tabContextHover_][0]) drawRect(cv, 2, 2 + tabContextHover_ * itemH, mainW - 2, 2 + (tabContextHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
            flush(cv);
            for (int i = 0; i < itemCount; ++i) if (items[i][0]) fontAtlas().drawText(items[i], 10.f, 6.f + i * itemH, 0.78f, 0.78f, 0.82f, 1.f);
        } else if (popupKind == PopupKind::Status) {
            int itemCount = (statusPopup_ == StatusPopup::Indent) ? 8 : std::min(syntaxLangCount, 18);
            std::vector<float> pv;
            drawRect(pv, 0, 0, mainW, mainH, 0.18f, 0.18f, 0.21f, 0.98f);
            drawRect(pv, 0, 0, mainW, 1, 0.3f, 0.3f, 0.35f, 1.f);
            if (popupSelected_ >= 0 && popupSelected_ < itemCount)
                drawRect(pv, 2, 2 + popupSelected_ * 24.f, mainW - 2, 2 + (popupSelected_ + 1) * 24.f, 0.25f, 0.30f, 0.45f, 1.f);
            flush(pv);
            if (statusPopup_ == StatusPopup::Indent) {
                const char* indentItems[] = {"Indent Using Spaces","Indent Using Tabs","Tab Width: 2","Tab Width: 4","Tab Width: 8","Convert Indentation to Spaces","Convert Indentation to Tabs","Detect Indentation"};
                for (int i = 0; i < itemCount; ++i) {
                    bool active = (i == 0 && !useTabs_) || (i == 1 && useTabs_) || (i == 2 && tabSize_ == 2) || (i == 3 && tabSize_ == 4) || (i == 4 && tabSize_ == 8);
                    if (active) fontAtlas().drawText("\xe2\x80\xa2", 8.f, 6.f + i * 24.f, 0.9f, 0.9f, 1.f, 1.f);
                    float b = active ? 0.9f : 0.7f;
                    fontAtlas().drawText(indentItems[i], 24.f, 6.f + i * 24.f, b, b, active ? 1.f : 0.75f, 1.f);
                }
            } else {
                for (int row = 0; row < itemCount; ++row) {
                    int i = popupScroll_ + row;
                    if (i >= syntaxLangCount) break;
                    float b = (syntax_->languageName() == syntaxLanguages[i]) ? 1.f : 0.7f;
                    if (syntax_->languageName() == syntaxLanguages[i]) fontAtlas().drawText("\xe2\x80\xa2", 8.f, 6.f + row * 24.f, 0.9f, 0.9f, 1.f, 1.f);
                    fontAtlas().drawText(syntaxLanguages[i], 24.f, 6.f + row * 24.f, b, b, b, 1.f);
                }
            }
        } else if (popupKind == PopupKind::CommandPalette) {
            std::vector<float> pv;
            float headerH = 38.f, rowH = 24.f;
            int total = (int)commandPalette_.results.size();
            int visible = std::min(12, total - commandPalette_.scroll);
            if (visible < 0) visible = 0;
            drawRect(pv, 0, 0, mainW, mainH, 0.15f, 0.15f, 0.18f, 0.98f);
            drawRect(pv, 0, 0, mainW, headerH, 0.12f, 0.12f, 0.14f, 1.f);
            int selectedRow = commandPalette_.selected - commandPalette_.scroll;
            if (selectedRow >= 0 && selectedRow < visible)
                drawRect(pv, 2.f, headerH + selectedRow * rowH, mainW - 8.f, headerH + (selectedRow + 1) * rowH, 0.25f, 0.30f, 0.45f, 1.f);
            if (total > 12) {
                float trackX = mainW - 6.f;
                float listH = 12.f * rowH;
                float thumbH = std::max(20.f, listH * (12.f / (float)total));
                float maxScroll = (float)std::max(1, total - 12);
                float thumbY = headerH + ((float)commandPalette_.scroll / maxScroll) * (listH - thumbH);
                drawRect(pv, trackX, thumbY, trackX + 4.f, thumbY + thumbH, 0.32f, 0.32f, 0.35f, 1.f);
            }
            flush(pv);
            fontAtlas().drawText(">", 12.f, 9.f, 0.70f, 0.72f, 0.78f, 1.f);
            fontAtlas().drawText("_", 22.f, 12.f, 0.70f, 0.72f, 0.78f, 1.f);
            fontAtlas().drawText(commandPalette_.query + "|", 42.f, 8.f, 0.84f, 0.84f, 0.86f, 1.f);
            std::string count = std::to_string(total) + " commands";
            float countW = fontAtlas().measureText(count);
            fontAtlas().drawText(count, mainW - countW - 14.f, 8.f, 0.46f, 0.46f, 0.50f, 1.f);
            for (int row = 0; row < visible; ++row) {
                int resultIndex = commandPalette_.scroll + row;
                int idx = commandPalette_.results[resultIndex];
                float b = resultIndex == commandPalette_.selected ? 0.98f : 0.72f;
                float y = headerH + 4.f + row * rowH;
                fontAtlas().drawText(paletteCommands[idx].label, 12.f, y, b, b, b + 0.03f, 1.f);
                if (paletteCommands[idx].shortcut[0]) {
                    float sw = fontAtlas().measureText(paletteCommands[idx].shortcut);
                    fontAtlas().drawText(paletteCommands[idx].shortcut, mainW - sw - 16.f, y, 0.48f, 0.48f, 0.52f, 1.f);
                }
            }
        } else if (popupKind == PopupKind::Autocomplete) {
            std::vector<float> av; float itemH = 22.f;
            drawRect(av, 0, 0, mainW, mainH, 0.17f, 0.19f, 0.23f, 0.98f);
            if (acSelected_ >= 0 && acSelected_ < (int)acItems_.size())
                drawRect(av, 2, 2 + acSelected_ * itemH, mainW - 2, 2 + (acSelected_ + 1) * itemH, 0.24f, 0.27f, 0.32f, 1.f);
            flush(av);
            for (int i = 0; i < (int)acItems_.size(); ++i) {
                float b = (i == acSelected_) ? 1.f : 0.7f;
                fontAtlas().drawText(acItems_[i], 8.f, 4.f + i * itemH, b, b, b, 1.f);
            }
        }
        GLRenderer::endFrame();
        SDL_GL_SwapWindow(popupWin_);
        SDL_GL_MakeCurrent(window_, glContext_);
        GLRenderer::resize(ww, wh);
    }
    if (popupWin_ && !hasPopup) hidePopupWindow();
}

void Application::shutdown() {
    saveSession();
    saveRecentFiles();
    sidebarWatchRunning_ = false;
    if (popupWin_) { SDL_DestroyWindow(popupWin_); popupWin_ = nullptr; }
    delete syntax_; delete statusbar_; delete minimap_; delete gutter_; delete titlebar_;
    fontAtlas().destroy(); GLRenderer::destroy();
    SDL_StopTextInput(); SDL_GL_DeleteContext(glContext_); SDL_DestroyWindow(window_); SDL_Quit();
}

int main(int argc, char** argv) { return Application::instance().run(argc, argv); }
