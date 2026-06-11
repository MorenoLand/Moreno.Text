#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "UI/Titlebar.h"
#include "UI/MenuBar.h"
#include "UI/SidebarMenu.h"
#include "Plugin/PluginHost.h"
#include "Settings/SettingsManager.h"
#include "Theme/ThemeEngine.h"
#include "Commands/KeyBindingManager.h"
#include "UI/Gutter.h"
#include "UI/Minimap.h"
#include "UI/StatusBar.h"
#include "Syntax/SyntaxHighlighter.h"
#include "Platform/Platform.h"
#include "Core/FileBackedBuffer.h"
#include "Core/Version.h"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <GL/glew.h>
#include <cstdio>
#include <cstring>
#include <iostream>
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
#include <memory>
#include <unordered_map>
#include <array>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#include <strings.h>
#elif !defined(_WIN32)
#include <unistd.h>
#include <strings.h>
#endif

#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

static constexpr const char* kTabContextItems[] = {
    "Close Tab",
    "Close Other Tabs",
    "Close Tabs to the Right",
    "Close Tabs to the Left",
    "Close Unmodified Tabs",
    "Close Unmodified Tabs to the Right",
    "Close Tabs With Deleted Files",
    "",
    "Split View",
    "",
    "New File",
    "Open File    Ctrl+O",
    "",
    "Diff With Current Tab..."
};

static float tabContextMenuWidth(FontAtlas& font);

#ifdef _WIN32
static constexpr ULONG_PTR MorenoTabDropCopyDataId = 0x4D545441; // MTTA
static constexpr const char* MorenoWindowPropName = "MorenoText.MainWindow";

struct MorenoDropTargetSearch {
    POINT point{};
    HWND own = nullptr;
    HWND found = nullptr;
};

static BOOL CALLBACK findMorenoDropTargetProc(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<MorenoDropTargetSearch*>(lParam);
    if (!search || hwnd == search->own || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (!GetPropA(hwnd, MorenoWindowPropName)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect) || !PtInRect(&rect, search->point)) return TRUE;
    int localY = search->point.y - rect.top;
    if (localY < 28 || localY > 82) return TRUE;
    search->found = hwnd;
    return FALSE;
}
#endif

static std::string readLargeFileWindow(FileBackedBuffer& buffer, size_t startLine, uint64_t maxBytes,
                                       uint64_t& startByte, uint64_t& endByte) {
    constexpr size_t maxLines = 20000;
    startLine = std::min(startLine, buffer.lineCount() > 0 ? buffer.lineCount() - 1 : 0);
    startByte = buffer.lineStart(startLine);
    endByte = startByte;
    size_t line = startLine;
    while (line < buffer.lineCount() && line < startLine + maxLines) {
        uint64_t lineEnd = buffer.lineEnd(line);
        if (lineEnd > startByte + maxBytes && line > startLine) break;
        endByte = std::max(endByte, lineEnd);
        ++line;
        if (endByte >= startByte + maxBytes) break;
    }
    if (endByte <= startByte) endByte = std::min<uint64_t>(buffer.fileSize(), startByte + maxBytes);
    if (endByte > startByte + maxBytes && startByte + maxBytes < buffer.fileSize()) endByte = startByte + maxBytes;
    return buffer.readOriginal(startByte, endByte - startByte);
}

struct ConsoleBuf : std::streambuf {
    std::string& buf;
    ConsoleBuf(std::string& b) : buf(b) {}
    int overflow(int c) override { if (c != EOF) buf += (char)c; return c; }
    std::streamsize xsputn(const char* s, std::streamsize n) override { buf.append(s, n); return n; }
};

struct CloseConfirmLayout {
    float mw, mh, mx, my, by, bh;
    float saveX, saveW;
    float dontSaveX, dontSaveW;
    float cancelX, cancelW;
};

static std::string fitText(FontAtlas& font, const std::string& text, float maxW) {
    if (maxW <= 0.f) return {};
    if (font.measureText(text) <= maxW) return text;
    std::string out = text;
    const std::string suffix = "...";
    float suffixW = font.measureText(suffix);
    while (!out.empty() && font.measureText(out) + suffixW > maxW) out.pop_back();
    return out.empty() ? suffix : out + suffix;
}

static CloseConfirmLayout makeCloseConfirmLayout(FontAtlas& font, float windowW, float windowH, const std::string& fileName) {
    const std::string prompt = "Save changes to " + fileName + " before closing?";
    float promptW = font.measureText(prompt);
    float saveW = std::max(96.f, font.measureText("Save") + 36.f);
    float dontSaveW = std::max(112.f, font.measureText("Don't Save") + 36.f);
    float cancelW = std::max(96.f, font.measureText("Cancel") + 36.f);
    float gap = 14.f;
    float buttonsW = saveW + dontSaveW + cancelW + gap * 2.f;
    float mw = std::max({420.f, promptW + 44.f, buttonsW + 44.f});
    mw = std::min(mw, std::max(260.f, windowW - 32.f));
    float mh = 118.f;
    float mx = (windowW - mw) / 2.f;
    float my = (windowH - mh) / 2.f;
    float by = my + 76.f;
    float buttonsX = mx + (mw - buttonsW) / 2.f;
    return {mw, mh, mx, my, by, 26.f, buttonsX, saveW, buttonsX + saveW + gap, dontSaveW, buttonsX + saveW + gap + dontSaveW + gap, cancelW};
}

SDL_HitTestResult hitTestCallback(SDL_Window*, const SDL_Point* area, void* userdata) {
    auto* app = static_cast<Application*>(userdata);
    return app->titlebar_ ? app->titlebar_->hitTest(area->x, area->y, app->window_) : SDL_HITTEST_NORMAL;
}

static int compareCaseInsensitive(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

static bool useExternalPopupWindow() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

static TabBuffer tabFromDetachedJson(const nlohmann::json& data) {
    TabBuffer tab;
    tab.fileName = data.value("file_name", "untitled");
    tab.filePath = data.value("file_path", "");
    tab.text = data.value("text", "");
    tab.dirty = data.value("dirty", true);
    tab.scrollY = data.value("scroll_y", 0.f);
    tab.pluginSyntax = data.value("syntax", "");
    tab.pluginColorScheme = data.value("color_scheme", "");
    tab.manualSyntaxName = data.value("manual_syntax", "");
    size_t cursor = data.value("cursor", 0ull);
    if (cursor > tab.text.size()) cursor = tab.text.size();
    tab.selections.emplace_back(cursor);
    return tab;
}

static nlohmann::json detachedJsonFromTab(const TabBuffer& tab) {
    nlohmann::json data;
    data["file_name"] = tab.fileName;
    data["file_path"] = tab.filePath;
    data["text"] = tab.text;
    data["dirty"] = tab.dirty;
    data["scroll_y"] = tab.scrollY;
    data["cursor"] = tab.selections.empty() ? 0 : tab.selections[0].cursor;
    data["syntax"] = tab.pluginSyntax;
    data["color_scheme"] = tab.pluginColorScheme;
    data["manual_syntax"] = tab.manualSyntaxName;
    return data;
}

static std::string lowerPathPart(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool startsWithText(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

static std::string inferSyntaxKey(const std::string& filePath, const std::string& fileName, const std::string& text) {
    std::string name = lowerPathPart(fileName.empty() ? std::filesystem::path(filePath).filename().string() : fileName);
    std::string path = lowerPathPart(filePath);
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    if (name == "makefile" || name == "gnumakefile" || name == "bsdmakefile") return "makefile";
    if (name == "dockerfile" || name.rfind("dockerfile.", 0) == 0) return "dockerfile";
    if (name == "cmakelists.txt" || ext == "cmake") return "cmake";
    if (name == "go.mod" || name == "go.sum") return "go";
    if (name == ".gitignore" || name == ".gitattributes" || name == ".gitmodules") return "git";
    if (ext == "plist") return "xml";
    if (ext == "gradle" || ext == "kts") return "kotlin";
    if (ext == "tsx" || ext == "ts") return "ts";
    if (ext == "jsx" || ext == "mjs" || ext == "cjs") return "js";
    if (ext.empty() && startsWithText(text, "#!")) {
        size_t end = text.find('\n');
        std::string shebang = lowerPathPart(text.substr(0, end == std::string::npos ? text.size() : end));
        if (shebang.find("python") != std::string::npos) return "py";
        if (shebang.find("node") != std::string::npos || shebang.find("deno") != std::string::npos) return "js";
        if (shebang.find("ruby") != std::string::npos) return "rb";
        if (shebang.find("perl") != std::string::npos) return "pl";
        if (shebang.find("php") != std::string::npos) return "php";
        if (shebang.find("bash") != std::string::npos || shebang.find("zsh") != std::string::npos || shebang.find("sh") != std::string::npos) return "sh";
    }
    return ext;
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
        "C:/Windows/Fonts/CascadiaMono.ttf","C:/Windows/Fonts/CascadiaCode.ttf","C:/Windows/Fonts/consola.ttf","C:/Windows/Fonts/cour.ttf","C:/Windows/Fonts/lucon.ttf",
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
    static ConsoleBuf coutBuf(consoleBuffer_), cerrBuf(consoleBuffer_);
    static std::streambuf* oldCout = std::cout.rdbuf(&coutBuf);
    static std::streambuf* oldCerr = std::cerr.rdbuf(&cerrBuf);
    (void)oldCout; (void)oldCerr;
    fs::create_directories(paths_.localDir);
    {
        std::ofstream clearLog(fs::path(paths_.localDir) / "Console.log", std::ios::binary | std::ios::trunc);
    }
    try {
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        for (auto& entry : fs::directory_iterator(bridge)) {
            if (entry.is_regular_file()) fs::remove(entry.path());
        }
    } catch (...) {}
    consoleBuffer_ += "DPI mode: per-monitor v2\n";
    consoleBuffer_ += std::string("startup, version: ") + MORENO_TEXT_VERSION + " channel: dev\n";
    consoleBuffer_ += "executable: " + paths_.exeDir + "/moreno_text.exe\n";
    consoleBuffer_ += "packages path: " + paths_.packagesDir + "\n";
    consoleBuffer_ += "state path: " + paths_.localDir + "\n";
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
    refreshPluginCommands();
    PluginHostPaths pluginPaths{paths_.exeDir, paths_.dataDir, paths_.packagesDir, paths_.installedPackagesDir, paths_.cacheDir, paths_.libDir};
    PluginHost::start(pluginPaths);
    selections_.emplace_back(0);
    TabBuffer initTab; initTab.fileName = "untitled"; initTab.selections.emplace_back(0);
    tabs_.push_back(std::move(initTab));
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) { std::cerr << "SDL_Init: " << SDL_GetError() << std::endl; return false; }
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
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
#ifdef _WIN32
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
    SDL_SysWMinfo wmInfo; SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window_, &wmInfo)) {
#ifdef _WIN32
        nativeWindowHandle_ = reinterpret_cast<void*>(wmInfo.info.win.window);
        SetPropA(wmInfo.info.win.window, MorenoWindowPropName, reinterpret_cast<HANDLE>(1));
        Platform::setRoundedCorners(nativeWindowHandle_, 8);
#endif
    }
    SDL_StartTextInput();
    if (argc > 2 && std::string(argv[1]) == "--detached-tab" && fs::exists(argv[2])) {
        tabs_.clear();
        selections_.clear();
        try {
            std::ifstream in(argv[2], std::ios::binary);
            nlohmann::json data;
            in >> data;
            in.close();
            tabs_.push_back(tabFromDetachedJson(data));
            activeTab_ = 0;
            loadTab(0);
            std::error_code ec;
            fs::remove(argv[2], ec);
        } catch (...) {
            newBuffer();
        }
    } else if (argc > 1 && fs::exists(argv[1])) {
        tabs_.clear();
        selections_.clear();
        openFile(argv[1]);
    }
    else if (!loadSession()) newBuffer();
    return true;
}

void Application::initPaths() {
#ifdef _WIN32
    wchar_t ep[MAX_PATH]; GetModuleFileNameW(nullptr, ep, MAX_PATH);
    paths_.exeDir = fs::path(ep).parent_path().string();
#elif __APPLE__
    char ep[PATH_MAX] = {};
    uint32_t size = sizeof(ep);
    if (_NSGetExecutablePath(ep, &size) == 0) paths_.exeDir = fs::weakly_canonical(ep).parent_path().string();
    else paths_.exeDir = fs::current_path().string();
#else
    char ep[4096] = {};
    ssize_t len = readlink("/proc/self/exe", ep, sizeof(ep) - 1);
    if (len > 0) { ep[len] = '\0'; paths_.exeDir = fs::path(ep).parent_path().string(); }
    else paths_.exeDir = fs::current_path().string();
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
            uintmax_t fileSize = 0;
            try { fileSize = fs::file_size(path); } catch (...) {}
            constexpr uintmax_t guardedLoadLimit = 16ull * 1024ull * 1024ull;
            std::string text;
            size_t guardedTotalLines = 0;
            uint64_t windowStartByte = 0;
            uint64_t windowEndByte = 0;
            std::shared_ptr<FileBackedBuffer> largeBuffer;
            if (fileSize > guardedLoadLimit) {
                largeBuffer = std::make_shared<FileBackedBuffer>();
                if (!largeBuffer->open(path)) continue;
                guardedTotalLines = largeBuffer->lineCount();
                text = readLargeFileWindow(*largeBuffer, 0, 64ull * 1024ull * 1024ull, windowStartByte, windowEndByte);
            } else {
                std::ostringstream ss; ss << tf.rdbuf(); text = ss.str();
            }
            TabBuffer tab;
            tab.text = std::move(text);
            tab.filePath = path;
            tab.fileName = fs::path(path).filename().string();
            tab.largeFileGuarded = fileSize > guardedLoadLimit;
            tab.largeFileSize = fileSize;
            tab.largeFileTotalLines = guardedTotalLines;
            tab.largeFileWindowStartByte = windowStartByte;
            tab.largeFileWindowEndByte = windowEndByte;
            tab.largeFileBuffer = std::move(largeBuffer);
            tab.manualSyntaxName = item.value("manualSyntax", "");
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
        std::string root = data.value("sidebar_root", "");
        if (!root.empty() && fs::exists(root)) loadFolder(root);
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
        data["active_tab"] = 0;
        if (!sidebarRoot_.empty()) data["sidebar_root"] = sidebarRoot_;
        data["tabs"] = nlohmann::json::array();
        for (size_t i = 0; i < tabs_.size(); ++i) {
            auto& tab = tabs_[i];
            if (tab.filePath.empty()) continue;
            if (tab.pluginOwned || tab.filePath.rfind("plugin://view/", 0) == 0) continue;
            nlohmann::json item;
            item["path"] = tab.filePath;
            item["scrollY"] = tab.scrollY;
            item["cursor"] = tab.selections.empty() ? 0 : tab.selections[0].cursor;
            if (!tab.manualSyntaxName.empty()) item["manualSyntax"] = tab.manualSyntaxName;
            if (i == activeTab_) data["active_tab"] = data["tabs"].size();
            data["tabs"].push_back(item);
        }
        std::ofstream f(fs::path(paths_.localDir) / "session.json", std::ios::binary);
        if (f) f << data.dump(2);
    } catch (...) {}
}

void Application::rebuildLineIndex() const {
    if (!lineIndexDirty_) return;
    lineStarts_.clear();
    lineStarts_.push_back(0);
    size_t pos = 0;
    while ((pos = textBuffer.find('\n', pos)) != std::string::npos) {
        ++pos;
        lineStarts_.push_back(pos);
    }
    lineIndexDirty_ = false;
}

size_t Application::lineStart(size_t pos) const { if (pos == 0) return 0; size_t p = textBuffer.rfind('\n', pos - 1); return p == std::string::npos ? 0 : p + 1; }
size_t Application::lineEnd(size_t pos) const { size_t p = textBuffer.find('\n', pos); return p == std::string::npos ? textBuffer.size() : p; }
size_t Application::lineStartForLine(size_t line) const {
    rebuildLineIndex();
    if (largeFileGuarded_) {
        if (line < largeFileWindowFirstLine_) return 0;
        line -= largeFileWindowFirstLine_;
    }
    if (line >= lineStarts_.size()) return textBuffer.size();
    return lineStarts_[line];
}
size_t Application::lineOfPos(size_t pos) const {
    rebuildLineIndex();
    pos = std::min(pos, textBuffer.size());
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), pos);
    size_t localLine = it == lineStarts_.begin() ? 0 : static_cast<size_t>((it - lineStarts_.begin()) - 1);
    return largeFileGuarded_ ? largeFileWindowFirstLine_ + localLine : localLine;
}
size_t Application::totalLines() const {
    rebuildLineIndex();
    if (largeFileGuarded_ && largeFileTotalLines_ > lineStarts_.size()) return largeFileTotalLines_;
    return std::max<size_t>(1, lineStarts_.size());
}
size_t Application::colOfPos(size_t pos) const { size_t ls = lineStart(pos); return pos - ls; }

void Application::ensureLargeFileWindowForLine(size_t line) {
    if (!largeFileGuarded_ || !largeFileBuffer_ || dirty_) return;
    rebuildLineIndex();
    size_t loadedLines = std::max<size_t>(1, lineStarts_.size());
    size_t windowEndLine = largeFileWindowFirstLine_ + loadedLines;
    if (line >= largeFileWindowFirstLine_ && line + 128 < windowEndLine) return;

    size_t startLine = line > 256 ? line - 256 : 0;
    uint64_t startByte = 0;
    uint64_t endByte = 0;
    std::string windowText = readLargeFileWindow(*largeFileBuffer_, startLine, 64ull * 1024ull * 1024ull, startByte, endByte);
    if (windowText.empty() && largeFileBuffer_->fileSize() > 0) return;

    textBuffer = std::move(windowText);
    largeFileWindowFirstLine_ = startLine;
    largeFileWindowStartByte_ = startByte;
    largeFileWindowEndByte_ = endByte;
    invalidateLineIndex();
    selections_.clear();
    selections_.emplace_back(0);
    scrollX_ = 0.f;
    maxLineWidthDirty_ = true;
    syntaxDirty_ = true;
    indentsDirty_ = true;
    if (activeTab_ < tabs_.size()) {
        tabs_[activeTab_].text = textBuffer;
        tabs_[activeTab_].largeFileWindowFirstLine = largeFileWindowFirstLine_;
        tabs_[activeTab_].largeFileWindowStartByte = largeFileWindowStartByte_;
        tabs_[activeTab_].largeFileWindowEndByte = largeFileWindowEndByte_;
    }
}

void Application::insertText(const std::string& text) {
    pushUndo();
    size_t oldPos = selections_[0].cursor;
    if (selections_[0].hasSelection()) deleteSelection();
    size_t pos = selections_[0].cursor;
    size_t startRow = lineOfPos(pos), startCol = pos - lineStart(pos);
    textBuffer.insert(pos, text);
    invalidateLineIndex();
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
    invalidateLineIndex();
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
        invalidateLineIndex();
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
    float viewH = std::max(1.f, (float)wh - statusbar_->height() - textOriginY);
    float contentH = (float)totalLines() * lineStep;
    float maxScroll = contentH > viewH ? contentH - viewH : 0.f;
    if (scrollY_ > maxScroll) scrollY_ = maxScroll;
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
        invalidateLineIndex();
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
    invalidateLineIndex();
    dirty_ = true;
    findAllMatches();
}

void Application::detectSyntax() {
    if (openFilePath_.rfind("plugin://view/", 0) == 0 && activeTab_ < tabs_.size() && !tabs_[activeTab_].pluginSyntax.empty()) {
        syntax_->setPluginSyntax(tabs_[activeTab_].pluginSyntax, tabs_[activeTab_].pluginColorScheme);
        syntaxDirty_ = true; indentsDirty_ = true;
        return;
    }
    if (activeTab_ < tabs_.size() && !tabs_[activeTab_].manualSyntaxName.empty()) {
        syntax_->setLanguageByName(tabs_[activeTab_].manualSyntaxName);
        syntaxDirty_ = true; indentsDirty_ = true;
        return;
    }
    std::string syntaxKey = inferSyntaxKey(openFilePath_, openFile_, textBuffer);
    syntax_->setLanguage(syntaxKey);
    syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::updateGitBranch() {
    std::string dir;
    if (!sidebarRoot_.empty()) dir = sidebarRoot_;
    else if (!openFilePath_.empty()) dir = fs::path(openFilePath_).parent_path().string();
    if (dir.empty()) { std::lock_guard<std::mutex> lock(gitBranchMutex_); gitBranch_.clear(); return; }
    if (gitBranchBusy_.exchange(true)) return;
    std::thread([this, dir] {
        std::string branch;
#ifdef _WIN32
        HANDLE hRead, hWrite; SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        CreatePipe(&hRead, &hWrite, &sa, 0); SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
        std::string cmd = "git -C \"" + dir + "\" rev-parse --abbrev-ref HEAD";
        STARTUPINFOA si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.hStdOutput = hWrite; si.hStdError = nullptr; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        char cmdBuf[512]; strncpy(cmdBuf, cmd.c_str(), sizeof(cmdBuf)-1); cmdBuf[sizeof(cmdBuf)-1] = 0;
        if (CreateProcessA(nullptr, cmdBuf, nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hWrite);
            char buf[256]; DWORD rd;
            while (ReadFile(hRead, buf, sizeof(buf)-1, &rd, nullptr) && rd) { buf[rd] = 0; branch += buf; }
            CloseHandle(hRead); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        } else { CloseHandle(hRead); CloseHandle(hWrite); }
#else
        std::string command = "git -C \"" + dir + "\" rev-parse --abbrev-ref HEAD 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) { gitBranchBusy_ = false; return; }
        char buf[256] = {}; while (fgets(buf, sizeof(buf), pipe)) branch += buf;
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
    std::string absPath;
    try { absPath = fs::absolute(path).string(); } catch (...) { absPath = path; }
    uintmax_t fileSize = 0;
    try { fileSize = fs::file_size(absPath); } catch (...) {}
    constexpr uintmax_t guardedLoadLimit = 16ull * 1024ull * 1024ull;
    std::string text;
    size_t guardedTotalLines = 0;
    uint64_t windowStartByte = 0;
    uint64_t windowEndByte = 0;
    std::shared_ptr<FileBackedBuffer> largeBuffer;
    if (fileSize > guardedLoadLimit) {
        largeBuffer = std::make_shared<FileBackedBuffer>();
        if (!largeBuffer->open(absPath)) return;
        guardedTotalLines = largeBuffer->lineCount();
        text = readLargeFileWindow(*largeBuffer, 0, 64ull * 1024ull * 1024ull, windowStartByte, windowEndByte);
    } else {
        std::ostringstream ss; ss << f.rdbuf(); text = ss.str();
    }
    TabBuffer tab; tab.text = std::move(text); tab.filePath = absPath; tab.fileName = fs::path(absPath).filename().string();
    tab.largeFileGuarded = fileSize > guardedLoadLimit;
    tab.largeFileSize = fileSize;
    tab.largeFileTotalLines = guardedTotalLines;
    tab.largeFileWindowStartByte = windowStartByte;
    tab.largeFileWindowEndByte = windowEndByte;
    tab.largeFileBuffer = std::move(largeBuffer);
    tab.selections.emplace_back(0);
    tabs_.push_back(std::move(tab)); activeTab_ = tabs_.size()-1;
    textBuffer = tabs_[activeTab_].text; openFilePath_ = absPath; openFile_ = tabs_[activeTab_].fileName;
    invalidateLineIndex();
    largeFileGuarded_ = tabs_[activeTab_].largeFileGuarded; largeFileSize_ = tabs_[activeTab_].largeFileSize; largeFileTotalLines_ = tabs_[activeTab_].largeFileTotalLines; largeFileWindowFirstLine_ = tabs_[activeTab_].largeFileWindowFirstLine; largeFileWindowStartByte_ = tabs_[activeTab_].largeFileWindowStartByte; largeFileWindowEndByte_ = tabs_[activeTab_].largeFileWindowEndByte; largeFileBuffer_ = tabs_[activeTab_].largeFileBuffer;
    dirty_ = false; selections_.clear(); selections_.emplace_back(0);
    scrollY_ = 0; scrollX_ = 0; maxLineWidthDirty_ = true; undoStack_.clear(); redoStack_.clear(); foldedRegions_.clear(); detectSyntax(); updateGitBranch();
    rememberRecentFile(absPath);
    saveSession();
}
void Application::saveFile() {
    if (sendPluginViewWindowCommand("save")) {
        dirty_ = false;
        if (activeTab_ < tabs_.size()) {
            tabs_[activeTab_].text = textBuffer;
            tabs_[activeTab_].dirty = false;
        }
        saveSession();
        return;
    }
    if (largeFileGuarded_) {
        if (!largeFileBuffer_ || openFilePath_.empty()) return;
        largeFileBuffer_->replaceOriginalRange(largeFileWindowStartByte_, largeFileWindowEndByte_, textBuffer);
        fs::path target(openFilePath_);
        fs::path tmp = target;
        tmp += ".morenotext-save";
        if (!largeFileBuffer_->saveTo(tmp)) {
            std::error_code cleanupEc;
            fs::remove(tmp, cleanupEc);
            return;
        }
        std::error_code ec;
        fs::rename(tmp, target, ec);
        if (ec) {
            fs::copy_file(tmp, target, fs::copy_options::overwrite_existing, ec);
            std::error_code cleanupEc;
            fs::remove(tmp, cleanupEc);
            if (ec) return;
        }
        largeFileBuffer_ = std::make_shared<FileBackedBuffer>();
        if (largeFileBuffer_->open(target)) {
            largeFileSize_ = largeFileBuffer_->fileSize();
            largeFileTotalLines_ = largeFileBuffer_->lineCount();
            largeFileWindowEndByte_ = largeFileWindowStartByte_ + static_cast<uint64_t>(textBuffer.size());
        }
        dirty_ = false;
        if (activeTab_ < tabs_.size()) {
            tabs_[activeTab_].text = textBuffer;
            tabs_[activeTab_].dirty = false;
            tabs_[activeTab_].largeFileBuffer = largeFileBuffer_;
            tabs_[activeTab_].largeFileSize = largeFileSize_;
            tabs_[activeTab_].largeFileTotalLines = largeFileTotalLines_;
            tabs_[activeTab_].largeFileWindowStartByte = largeFileWindowStartByte_;
            tabs_[activeTab_].largeFileWindowEndByte = largeFileWindowEndByte_;
        }
        rememberRecentFile(openFilePath_);
        saveSession();
        updateGitBranch();
        return;
    }
    if (openFilePath_.empty()) { saveFileAs(); return; }
    std::ofstream f(openFilePath_, std::ios::binary);
    if (!f) return;
    f << textBuffer;
    dirty_ = false;
    rememberRecentFile(openFilePath_);
    saveSession();
    updateGitBranch();
}
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
        if (f && !tab.largeFileGuarded) { f << tab.text; tab.dirty = false; rememberRecentFile(tab.filePath); }
    }
    loadTab(activeTab_);
}
void Application::revertFile() {
    if (openFilePath_.empty()) return;
    std::ifstream f(openFilePath_, std::ios::binary);
    if (!f) return;
    uintmax_t fileSize = 0;
    try { fileSize = fs::file_size(openFilePath_); } catch (...) {}
    constexpr uintmax_t guardedLoadLimit = 16ull * 1024ull * 1024ull;
    if (fileSize > guardedLoadLimit) {
        largeFileBuffer_ = std::make_shared<FileBackedBuffer>();
        if (!largeFileBuffer_->open(openFilePath_)) return;
        largeFileSize_ = largeFileBuffer_->fileSize();
        largeFileTotalLines_ = largeFileBuffer_->lineCount();
        largeFileWindowFirstLine_ = 0;
        textBuffer = readLargeFileWindow(*largeFileBuffer_, 0, 64ull * 1024ull * 1024ull, largeFileWindowStartByte_, largeFileWindowEndByte_);
        largeFileGuarded_ = true;
    } else {
        std::ostringstream ss; ss << f.rdbuf();
        textBuffer = ss.str();
        largeFileGuarded_ = false; largeFileSize_ = 0; largeFileTotalLines_ = 0; largeFileWindowFirstLine_ = 0; largeFileWindowStartByte_ = 0; largeFileWindowEndByte_ = 0; largeFileBuffer_.reset();
    }
    invalidateLineIndex(); dirty_ = false; maxLineWidthDirty_ = true; syntaxDirty_ = true; indentsDirty_ = true; selections_.clear(); selections_.emplace_back(std::min(selections_.empty() ? 0 : selections_[0].cursor, textBuffer.size()));
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
    textBuffer.clear(); invalidateLineIndex(); openFilePath_.clear(); openFile_ = "untitled"; dirty_ = false;
    selections_.clear(); selections_.emplace_back(0); scrollY_ = 0; scrollX_ = 0; largeFileGuarded_ = false; largeFileSize_ = 0; largeFileTotalLines_ = 0; largeFileWindowFirstLine_ = 0; largeFileWindowStartByte_ = 0; largeFileWindowEndByte_ = 0; largeFileBuffer_.reset(); maxLineWidthDirty_ = true; undoStack_.clear(); redoStack_.clear(); foldedRegions_.clear(); detectSyntax(); saveSession();
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
    auto byName = [](const SidebarNode& a, const SidebarNode& b) { return compareCaseInsensitive(a.name.c_str(), b.name.c_str()) < 0; };
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
    if (!sidebarTree_.name.empty()) sidebarContentH_ += rowH;
    std::function<void(SidebarNode&)> countNode = [&](SidebarNode& node) {
        if (&node != &sidebarTree_) sidebarContentH_ += rowH;
        if (node.folder && node.expanded) for (auto& child : node.children) countNode(child);
    };
    countNode(sidebarTree_);
    float viewH = windowH - statusbarH - titlebarH - 6.f;
    float maxScroll = std::max(0.f, sidebarContentH_ - viewH);
    if (sidebarScrollY_ > maxScroll) sidebarScrollY_ = maxScroll;
    if (!sidebarTree_.name.empty() && y + rowH >= titlebarH && y <= windowH - statusbarH) {
        addSolidRect(v, 2.f, y - 3.f, sidebarWidth_ - 4.f, y + rowH - 3.f, 0.16f, 0.18f, 0.22f, 1.f);
        addSolidRect(v, 8.f + 12.f, y + 3.f, 8.f + 20.f, y + 13.f, 0.86f, 0.66f, 0.22f, 1.f);
        textDraws.push_back({"v", 8.f, y, 0.55f, 0.55f, 0.58f});
        textDraws.push_back({fitSidebarText(sidebarTree_.name, 8.f + 26.f), 8.f + 26.f, y, 0.88f, 0.88f, 0.91f});
    }
    y += rowH;
    std::function<void(SidebarNode&)> drawNode = [&](SidebarNode& node) {
        if (&node != &sidebarTree_) {
            float rowY = y; y += rowH;
            bool visible = rowY + rowH >= titlebarH && rowY <= windowH - statusbarH;
            if (visible) {
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
        }
        if (node.folder && node.expanded) for (auto& child : node.children) drawNode(child);
    };
    drawNode(sidebarTree_);
    if (sidebarContentH_ > viewH) {
        float thumbH = std::max(20.f, viewH * (viewH / sidebarContentH_));
        float thumbY = titlebarH + (sidebarScrollY_ / maxScroll) * (viewH - thumbH);
        addSolidRect(v, sidebarWidth_ - 5.f, thumbY, sidebarWidth_ - 1.f, thumbY + thumbH, 0.29f, 0.29f, 0.29f, 1.f);
    }
    glEnable(GL_SCISSOR_TEST); glScissor(0, (int)statusbarH, (int)sidebarWidth_, (int)(windowH - titlebarH - statusbarH));
    if (!v.empty()) {
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, v.size()*sizeof(float), v.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
    }
    for (const auto& td : textDraws) font.drawText(td.text, td.x, td.y, td.r, td.g, td.b, 1.f);
    glDisable(GL_SCISSOR_TEST);
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
    if (e.type == SDL_MOUSEMOTION && sidebarContextOpen_) {
        float mx = (float)e.motion.x, my = (float)e.motion.y, itemH = 24.f, popW = 260.f;
        float popH = 4.f + sidebarContextItems_.size() * itemH;
        sidebarContextHover_ = (mx >= sidebarContextX_ && mx <= sidebarContextX_ + popW && my >= sidebarContextY_ && my <= sidebarContextY_ + popH) ? (int)((my - sidebarContextY_ - 2.f) / itemH) : -1;
        if (sidebarContextHover_ >= 0 && sidebarContextHover_ < (int)sidebarContextItems_.size() && sidebarContextItems_[sidebarContextHover_].separator) sidebarContextHover_ = -1;
        return true;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && sidebarContextOpen_) {
        float mx = (float)e.button.x, my = (float)e.button.y, itemH = 24.f, popW = 260.f;
        float popH = 4.f + sidebarContextItems_.size() * itemH;
        if (mx >= sidebarContextX_ && mx <= sidebarContextX_ + popW && my >= sidebarContextY_ && my <= sidebarContextY_ + popH) {
            int idx = (int)((my - sidebarContextY_ - 2.f) / itemH);
            if (idx >= 0 && idx < (int)sidebarContextItems_.size() && !sidebarContextItems_[idx].separator) {
                int action = sidebarContextItems_[idx].action;
                std::string path = sidebarContextNode_.path;
                if (action == 2) openFolderDialog();
                else if (action == 4) SDL_SetClipboardText(path.c_str());
#ifdef _WIN32
                else if (action == 5 || action == 11) {
                    std::string dir = sidebarContextNode_.folder ? path : fs::path(path).parent_path().string();
                    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else if (action == 10) {
                    sidebarRoot_.clear(); sidebarTree_ = {}; sidebarVisible_ = false; saveSession();
                }
#endif
            }
            sidebarContextOpen_ = false;
            return true;
        }
        sidebarContextOpen_ = false;
        return true;
    }
    auto openSidebarContext = [&](SidebarNode& node, float mx, float my, bool root) {
        sidebarContextNode_ = node;
        sidebarContextItems_ = sidebarMenuItems(node.folder, root);
        sidebarContextX_ = mx;
        sidebarContextY_ = my;
        sidebarContextHover_ = -1;
        sidebarContextOpen_ = true;
    };
    auto hitSidebarNode = [&](float my, SidebarNode*& outNode, bool& root) {
        float rowH = 22.f, y = titlebarH + 6.f - sidebarScrollY_;
        outNode = nullptr; root = false;
        if (!sidebarTree_.name.empty()) {
            if (my >= y - 3.f && my < y + rowH - 3.f) { outNode = &sidebarTree_; root = true; return true; }
            y += rowH;
        }
        std::function<bool(SidebarNode&)> hitNode = [&](SidebarNode& node) -> bool {
            if (&node != &sidebarTree_) {
                float rowY = y; y += rowH;
                if (my >= rowY - 3.f && my < rowY + rowH - 3.f) { outNode = &node; return true; }
            }
            if (node.folder && node.expanded) for (auto& child : node.children) if (hitNode(child)) return true;
            return false;
        };
        return hitNode(sidebarTree_);
    };
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 3 && e.button.x <= sidebarWidth_ && e.button.y >= titlebarH && e.button.y <= windowH - statusbarH) {
        SidebarNode* node = nullptr; bool root = false;
        if (hitSidebarNode((float)e.button.y, node, root) && node) openSidebarContext(*node, (float)e.button.x, (float)e.button.y, root);
        return true;
    }
    if (e.type != SDL_MOUSEBUTTONDOWN || e.button.button != 1 || e.button.x > sidebarWidth_ || e.button.y < titlebarH || e.button.y > windowH - statusbarH) return false;
    float rowH = 22.f, y = titlebarH + 6.f - sidebarScrollY_, mx = (float)e.button.x, my = (float)e.button.y;
    if (!sidebarTree_.name.empty()) {
        if (my >= y - 3.f && my < y + rowH - 3.f) { sidebarTree_.expanded = !sidebarTree_.expanded; return true; }
        y += rowH;
    }
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
void Application::doUndo() { if (undoStack_.empty()) return; redoStack_.push_back({textBuffer, selections_[0].cursor, std::chrono::steady_clock::now()}); textBuffer = undoStack_.back().text; invalidateLineIndex(); selections_.clear(); selections_.emplace_back(undoStack_.back().cursorPos); undoStack_.pop_back(); dirty_ = true; }
void Application::doRedo() { if (redoStack_.empty()) return; undoStack_.push_back({textBuffer, selections_[0].cursor, std::chrono::steady_clock::now()}); textBuffer = redoStack_.back().text; invalidateLineIndex(); selections_.clear(); selections_.emplace_back(redoStack_.back().cursorPos); redoStack_.pop_back(); dirty_ = true; }

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
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
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
        size_t targetLine = largeFileGuarded_ ? largeFileWindowFirstLine_ + li : li;
        if (targetLine >= lineIndents_.size()) break;
        lineIndents_[targetLine] = (extra > indent) ? (indent + (extra - indent) * tabSize_) : indent;
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
    if (largeFileGuarded_) {
        maxLineWidth_ = fontAtlas().measureText(" ") * 240.f;
        return;
    }
    size_t pos = 0;
    while (pos <= textBuffer.size()) {
        size_t end = textBuffer.find('\n', pos);
        if (end == std::string::npos) end = textBuffer.size();
        size_t len = end - pos;
        float width = len > 8192 ? static_cast<float>(len) * fontAtlas().getGlyph(' ').advance : fontAtlas().measureText(std::string_view(textBuffer.data() + pos, len));
        maxLineWidth_ = std::max(maxLineWidth_, width);
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
    tab.foldedRegions = foldedRegions_; tab.dirty = dirty_; tab.largeFileGuarded = largeFileGuarded_; tab.largeFileSize = largeFileSize_; tab.largeFileTotalLines = largeFileTotalLines_; tab.largeFileWindowFirstLine = largeFileWindowFirstLine_; tab.largeFileWindowStartByte = largeFileWindowStartByte_; tab.largeFileWindowEndByte = largeFileWindowEndByte_; tab.largeFileBuffer = largeFileBuffer_; tab.desiredCursorX = desiredCursorX_;
    if (activeGroup_ < editorGroups_.size()) editorGroups_[activeGroup_].tab = activeTab_;
}

void Application::loadTab(size_t index) {
    if (index >= tabs_.size()) return;
    auto& tab = tabs_[index];
    textBuffer = tab.text; invalidateLineIndex(); openFilePath_ = tab.filePath; openFile_ = tab.fileName;
    selections_ = tab.selections; scrollY_ = tab.scrollY;
    undoStack_ = std::move(tab.undoStack); redoStack_ = std::move(tab.redoStack);
    foldedRegions_ = tab.foldedRegions; dirty_ = tab.dirty; desiredCursorX_ = tab.desiredCursorX;
    largeFileGuarded_ = tab.largeFileGuarded; largeFileSize_ = tab.largeFileSize; largeFileTotalLines_ = tab.largeFileTotalLines; largeFileWindowFirstLine_ = tab.largeFileWindowFirstLine; largeFileWindowStartByte_ = tab.largeFileWindowStartByte; largeFileWindowEndByte_ = tab.largeFileWindowEndByte; largeFileBuffer_ = tab.largeFileBuffer;
    activeTab_ = index; scrollX_ = 0; maxLineWidthDirty_ = true; detectSyntax(); updateGitBranch();
    if (activeGroup_ < editorGroups_.size()) editorGroups_[activeGroup_].tab = activeTab_;
}

void Application::normalizeEditorGroups() {
    if (tabs_.empty()) {
        editorGroups_.clear();
        activeGroup_ = 0;
        return;
    }
    if (editorGroups_.empty()) editorGroups_.push_back({std::min(activeTab_, tabs_.size() - 1)});
    for (auto& group : editorGroups_) {
        if (group.tab >= tabs_.size()) group.tab = tabs_.size() - 1;
    }
    editorGroups_.erase(std::remove_if(editorGroups_.begin(), editorGroups_.end(), [&](const EditorGroup& group) {
        return group.tab >= tabs_.size();
    }), editorGroups_.end());
    if (editorGroups_.empty()) editorGroups_.push_back({std::min(activeTab_, tabs_.size() - 1)});
    if (activeGroup_ >= editorGroups_.size()) activeGroup_ = editorGroups_.size() - 1;
    editorGroups_[activeGroup_].tab = activeTab_;
}

void Application::collapseEditorGroupsTo(size_t tabIndex) {
    if (tabIndex >= tabs_.size()) return;
    editorGroups_.clear();
    editorGroups_.push_back({tabIndex});
    activeGroup_ = 0;
}

void Application::addEditorGroupForTab(size_t tabIndex) {
    if (tabIndex >= tabs_.size()) return;
    saveCurrentTab();
    normalizeEditorGroups();
    for (size_t i = 0; i < editorGroups_.size(); ++i) {
        if (editorGroups_[i].tab == tabIndex) {
            activeGroup_ = i;
            loadTab(tabIndex);
            return;
        }
    }
    editorGroups_.push_back({tabIndex});
    activeGroup_ = editorGroups_.size() - 1;
    loadTab(tabIndex);
}

void Application::switchToTab(size_t index) {
    if (index >= tabs_.size() || index == activeTab_) return;
    if (pluginQuickPanel_.active) finishPluginQuickPanel(-1);
    closeAllPopups();
    saveCurrentTab(); loadTab(index); normalizeEditorGroups(); saveSession();
}

void Application::closeTabNow(size_t index) {
    if (index >= tabs_.size()) return;
    bool pluginTab = tabs_[index].pluginOwned || tabs_[index].filePath.rfind("plugin://view/", 0) == 0;
    if (pluginTab) {
        try {
            int id = std::stoi(tabs_[index].filePath.substr(std::string("plugin://view/").size()));
            closedPluginViewIds_.insert(id);
            notifyPluginViewClosed(id);
        } catch (...) {}
    }
    if (!pluginTab && !tabs_[index].filePath.empty()) {
        closedTabStack_.push_back(tabs_[index].filePath);
        if (closedTabStack_.size() > 10) closedTabStack_.erase(closedTabStack_.begin());
    }
    tabs_.erase(tabs_.begin() + index);
    for (auto& group : editorGroups_) {
        if (group.tab > index) --group.tab;
        else if (group.tab == index) group.tab = activeTab_ >= tabs_.size() ? tabs_.size() - 1 : activeTab_;
    }
    if (tabs_.empty()) { newBuffer(); return; }
    if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.size()-1;
    normalizeEditorGroups();
    loadTab(activeTab_);
    saveSession();
}

bool Application::importDetachedTabFromFile(const std::string& path) {
    if (path.empty() || !fs::exists(path)) return false;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        nlohmann::json data;
        in >> data;
        in.close();
        saveCurrentTab();
        tabs_.push_back(tabFromDetachedJson(data));
        activeTab_ = tabs_.size() - 1;
        loadTab(activeTab_);
        normalizeEditorGroups();
        updateTitle();
        std::error_code ec;
        fs::remove(path, ec);
        return !fs::exists(path);
    } catch (...) {
        return false;
    }
}

void Application::closeTab(size_t index) {
    saveCurrentTab();
    if (index >= tabs_.size()) return;
    if (tabs_[index].pluginOwned || tabs_[index].filePath.rfind("plugin://view/", 0) == 0) { closeTabNow(index); return; }
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

bool Application::detachTabToExistingWindow(size_t index) {
#ifdef _WIN32
    if (index >= tabs_.size()) return false;
    int globalX = 0, globalY = 0;
    SDL_GetGlobalMouseState(&globalX, &globalY);
    HWND own = reinterpret_cast<HWND>(nativeWindowHandle_);
    MorenoDropTargetSearch search{};
    search.point = POINT{globalX, globalY};
    search.own = own;
    EnumWindows(findMorenoDropTargetProc, reinterpret_cast<LPARAM>(&search));
    HWND target = search.found;
    if (!target) return false;

    fs::path detachDir = fs::path(paths_.localDir) / "DetachedTabs";
    fs::create_directories(detachDir);
    fs::path handoff = detachDir / (std::to_string(SDL_GetTicks()) + "-" + std::to_string(index) + "-merge.json");
    {
        std::ofstream out(handoff, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << detachedJsonFromTab(tabs_[index]).dump();
    }

    std::string handoffText = handoff.string();
    COPYDATASTRUCT cds{};
    cds.dwData = MorenoTabDropCopyDataId;
    cds.cbData = static_cast<DWORD>(handoffText.size() + 1);
    cds.lpData = handoffText.data();
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutA(target, WM_COPYDATA, reinterpret_cast<WPARAM>(own), reinterpret_cast<LPARAM>(&cds),
                             SMTO_ABORTIFHUNG, 1000, &result)) {
        std::error_code ec;
        fs::remove(handoff, ec);
        return false;
    }

    for (int i = 0; i < 20 && fs::exists(handoff); ++i) SDL_Delay(10);
    if (!fs::exists(handoff)) return true;
    std::error_code ec;
    fs::remove(handoff, ec);
#endif
    return false;
}

bool Application::detachTabToNewWindow(size_t index) {
    saveCurrentTab();
    if (index >= tabs_.size()) return false;
    auto& tab = tabs_[index];
    if (tab.pluginOwned || tab.filePath.rfind("plugin://view/", 0) == 0) {
        consoleBuffer_ += "[tabs] Cannot tear out plugin-owned tabs yet\n";
        return false;
    }
    if (detachTabToExistingWindow(index)) {
        if (tabs_.size() == 1) {
            tabs_.erase(tabs_.begin() + index);
            activeTab_ = 0;
            running_ = false;
            return true;
        }
        closeTabNow(index);
        return true;
    }
    fs::path exe = fs::path(paths_.exeDir) / "moreno_text.exe";
    if (!fs::exists(exe)) return false;
    fs::path detachDir = fs::path(paths_.localDir) / "DetachedTabs";
    fs::create_directories(detachDir);
    fs::path handoff = detachDir / (std::to_string(SDL_GetTicks()) + "-" + std::to_string(index) + ".json");
    {
        std::ofstream out(handoff, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << detachedJsonFromTab(tab).dump();
    }
#ifdef _WIN32
    std::string command = "\"" + exe.string() + "\" --detached-tab \"" + handoff.string() + "\"";
    STARTUPINFOA si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd(command.begin(), command.end());
    cmd.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, paths_.exeDir.c_str(), &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#elif defined(__APPLE__) || defined(__linux__)
    std::string command = "\"" + exe.string() + "\" --detached-tab \"" + handoff.string() + "\" &";
    if (std::system(command.c_str()) != 0) return false;
#else
    return false;
#endif
    if (tabs_.size() == 1) {
        tabs_.erase(tabs_.begin() + index);
        activeTab_ = 0;
        running_ = false;
        return true;
    }
    closeTabNow(index);
    return true;
}

void Application::drawTabBar(FontAtlas& font, float windowW, float titlebarH) {
    auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        solidVerts_.insert(solidVerts_.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    float barY = titlebarH;
    constexpr float tabStripInset = 8.f;
    constexpr float scrollBtnW = 22.f;
    constexpr float allFilesW = 38.f;
    float menuX = std::max(tabStripInset, windowW - allFilesW);
    float initialVisibleW = std::max(0.f, menuX - tabStripInset);
    ar(0, barY, windowW, barY + tabBarH_, 0.25f, 0.25f, 0.28f, 1.f);
    constexpr float tabTextPadL = 12.f;
    constexpr float tabActionPadR = 14.f;
    constexpr float tabActionW = 22.f;
    constexpr float tabTextGap = 10.f;
    auto tabWidthForLabel = [&](const std::string& label) {
        return std::max(font.measureText(label) + tabTextPadL + tabTextGap + tabActionW + tabActionPadR, 58.f);
    };
    auto roundedTab = [&](float x0, float x1, float r, float g, float b, float a, bool active) {
        float y0 = barY + 3.f;
        float y1 = barY + tabBarH_;
        ar(x0 + 7.f, y0, x1 - 7.f, y0 + 1.f, r, g, b, a);
        ar(x0 + 5.f, y0 + 1.f, x1 - 5.f, y0 + 2.f, r, g, b, a);
        ar(x0 + 3.f, y0 + 2.f, x1 - 3.f, y0 + 4.f, r, g, b, a);
        ar(x0 + 1.f, y0 + 4.f, x1 - 1.f, y0 + 6.f, r, g, b, a);
        ar(x0, y0 + 6.f, x1, y1, r, g, b, a);
        if (active) {
            ar(x0 + 5.f, y0, x1 - 5.f, y0 + 1.f, std::min(r + 0.08f, 1.f), std::min(g + 0.08f, 1.f), std::min(b + 0.08f, 1.f), 0.65f);
            ar(x0, y1 - 1.f, x1, y1, r, g, b, a);
        }
    };
    float totalTabsW = 0.f;
    for (auto& tab : tabs_) {
        std::string label = tab.fileName.empty() ? "untitled" : tab.fileName;
        totalTabsW += tabWidthForLabel(label);
    }
    bool hasOverflow = totalTabsW > initialVisibleW;
    float scrollAreaW = hasOverflow ? scrollBtnW * 2.f : 0.f;
    float scrollAreaX = tabStripInset;
    float tabAreaX = tabStripInset + scrollAreaW;
    float visibleW = std::max(0.f, menuX - tabAreaX);
    float maxTabScroll = totalTabsW > visibleW ? totalTabsW - visibleW : 0.f;
    if (tabScrollX_ > maxTabScroll) tabScrollX_ = maxTabScroll;
    if (tabScrollX_ < 0.f) tabScrollX_ = 0.f;
    float tx = tabAreaX - tabScrollX_;
    struct TabDrawLabel { size_t index; float x; std::string text; };
    std::vector<TabDrawLabel> labels;
    labels.reserve(tabs_.size());
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
        float tw = tabWidthForLabel(label);
        if (tx + tw < tabAreaX) { tx += tw; continue; }
        if (tx > menuX) break;
        bool groupedTab = false;
        for (auto& group : editorGroups_) if (group.tab == i) { groupedTab = true; break; }
        if (i == activeTab_) {
            roundedTab(tx, tx + tw, 0.18f, 0.19f, 0.22f, 1.f, true);
        } else if (groupedTab) {
            roundedTab(tx, tx + tw, 0.23f, 0.24f, 0.27f, 0.75f, false);
        } else if (i == tabHoverIndex_) {
            roundedTab(tx, tx + tw, 0.24f, 0.25f, 0.28f, 0.55f, false);
        }
        labels.push_back({i, tx, label});
        tx += tw;
    }
    if (hasOverflow) {
        for (int i = 0; i < 2; ++i) {
            float x = scrollAreaX + i * scrollBtnW;
            ar(x, barY, x + scrollBtnW, barY + tabBarH_, 0.21f, 0.21f, 0.24f, 1.f);
        }
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
        float tw = tabWidthForLabel(label.text);
        float actionCenterX = lx + tw - tabActionPadR - tabActionW * 0.5f;
        bool closeHover = label.index == tabHoverIndex_ && mouseX_ >= (int)(actionCenterX - 10.f) && mouseX_ <= (int)(actionCenterX + 10.f) &&
            mouseY_ >= (int)(barY + 6.f) && mouseY_ <= (int)(barY + tabBarH_ - 4.f);
        float textClipRight = actionCenterX - tabTextGap;
        float clipLeft = std::max(lx + tabTextPadL, tabAreaX);
        float clipRight = std::min(textClipRight, menuX);
        if (clipRight > clipLeft) {
            font.drawTextClipped(label.text, lx + tabTextPadL, textY, clipLeft, clipRight, textBright, textBright, textBright+0.05f, 1.f);
        }
        if (closeHover) {
            font.drawText("\xc3\x97", actionCenterX - 4.f, textY, 0.55f, 0.55f, 0.60f, 1.f);
        } else {
            if (tabs_[label.index].dirty) font.drawText("\xe2\x80\xa2", actionCenterX - 4.f, textY + 1.f, 0.95f, 0.72f, 0.27f, 1.f);
            else font.drawText("\xe2\x80\xa2", actionCenterX - 4.f, textY + 1.f, 0.48f, 0.50f, 0.56f, 0.95f);
        }
    }
    flushTabSolids();
    float controlY = barY + 8.f;
    tabChevronX_ = std::max(0.f, windowW - 260.f - 4.f);
    if (hasOverflow) {
        float leftBright = tabScrollX_ > 0.f ? 0.78f : 0.38f;
        float rightBright = tabScrollX_ < maxTabScroll ? 0.78f : 0.38f;
        font.drawText("<", scrollAreaX + 7.f, controlY, leftBright, leftBright, leftBright + 0.03f, 1.f);
        font.drawText(">", scrollAreaX + scrollBtnW + 7.f, controlY, rightBright, rightBright, rightBright + 0.03f, 1.f);
    }
    font.drawText("v", menuX + 15.f, controlY, 0.75f, 0.75f, 0.78f, 1.f);
    if (!tabDropdownOpen_ && mouseX_ >= (int)menuX && mouseX_ < (int)windowW && mouseY_ >= (int)barY && mouseY_ < (int)(barY + tabBarH_)) {
        std::string tip = "All Files";
        float tipW = font.measureText(tip) + 18.f;
        float tipH = 24.f;
        float tipX = std::clamp(menuX - tipW + allFilesW, 4.f, std::max(4.f, windowW - tipW - 4.f));
        float tipY = barY + tabBarH_ + 4.f;
        ar(tipX, tipY, tipX + tipW, tipY + tipH, 0.12f, 0.12f, 0.14f, 0.96f);
        flushTabSolids();
        font.drawText(tip, tipX + 9.f, tipY + 5.f, 0.84f, 0.84f, 0.87f, 1.f);
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
        int itemCount = tabContextIndex_ != activeTab_ ? 14 : 13;
        float itemH = 24.f, popW = tabContextMenuWidth(font), popH = 4.f + itemCount * itemH;
        std::vector<float> cv;
        auto cr = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            cv.insert(cv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        cr(tabContextX_, tabContextY_, tabContextX_ + popW, tabContextY_ + popH, 0.17f, 0.17f, 0.20f, 0.98f);
        if (tabContextHover_ >= 0 && kTabContextItems[tabContextHover_][0]) cr(tabContextX_ + 2, tabContextY_ + 2 + tabContextHover_ * itemH, tabContextX_ + popW - 2, tabContextY_ + 2 + (tabContextHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, cv.size()*sizeof(float), cv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(cv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        for (int i = 0; i < itemCount; ++i) {
            if (!kTabContextItems[i][0]) { std::vector<float> sv; cr(tabContextX_ + 8.f, tabContextY_ + 13.f + i * itemH, tabContextX_ + popW - 8.f, tabContextY_ + 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f); continue; }
            bool greyed = (i == 2 && tabContextIndex_ >= tabs_.size() - 1) || (i == 3 && tabContextIndex_ == 0);
            float alpha = greyed ? 0.4f : 1.f;
            font.drawText(kTabContextItems[i], tabContextX_ + 10.f, tabContextY_ + 6.f + i * itemH, 0.78f * alpha, 0.78f * alpha, 0.82f * alpha, alpha);
        }
    }
}

bool Application::handleTabBarEvent(const SDL_Event& e, float windowW, float titlebarH) {
    float barY = titlebarH;
    constexpr float tabStripInset = 8.f;
    constexpr float scrollBtnW = 22.f;
    constexpr float allFilesW = 38.f;
    float menuX = std::max(tabStripInset, windowW - allFilesW);
    float initialVisibleW = std::max(0.f, menuX - tabStripInset);
    constexpr float tabTextPadL = 12.f;
    constexpr float tabActionPadR = 14.f;
    constexpr float tabActionW = 22.f;
    constexpr float tabTextGap = 10.f;
    auto tabWidthForLabel = [&](const std::string& label) {
        return std::max(fontAtlas().measureText(label) + tabTextPadL + tabTextGap + tabActionW + tabActionPadR, 58.f);
    };
    float totalTabsW = 0.f;
    for (auto& tab : tabs_) {
        std::string label = tab.fileName.empty() ? "untitled" : tab.fileName;
        totalTabsW += tabWidthForLabel(label);
    }
    bool hasOverflow = totalTabsW > initialVisibleW;
    float scrollAreaW = hasOverflow ? scrollBtnW * 2.f : 0.f;
    float scrollAreaX = tabStripInset;
    float tabAreaX = tabStripInset + scrollAreaW;
    float visibleW = std::max(0.f, menuX - tabAreaX);
    float maxTabScroll = totalTabsW > visibleW ? totalTabsW - visibleW : 0.f;
    if (consoleOpen_ && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        int wh; SDL_GetWindowSize(window_, nullptr, &wh);
        float sbH = statusbar_->height();
        float consoleTop = (float)wh - sbH - consoleH_;
        if ((float)e.button.y >= consoleTop - 4.f && (float)e.button.y <= consoleTop + 4.f) { consoleResizing_ = true; return true; }
    }
    if (consoleResizing_ && e.type == SDL_MOUSEMOTION) {
        int wh; SDL_GetWindowSize(window_, nullptr, &wh);
        float sbH = statusbar_->height();
        float newH = (float)wh - sbH - (float)e.motion.y;
        consoleH_ = std::clamp(newH, 60.f, (float)wh * 0.75f);
        return true;
    }
    if (consoleResizing_ && e.type == SDL_MOUSEBUTTONUP && e.button.button == 1) { consoleResizing_ = false; return true; }
    if (e.type == SDL_MOUSEMOTION && tabDragging_) {
        float dx = (float)e.motion.x - tabDragStartX_;
        if (std::abs(dx) > 4.f) {
            float dropX = (float)e.motion.x - tabAreaX + tabScrollX_;
            float tx = 0.f;
            size_t newIdx = tabs_.size();
            for (size_t i = 0; i < tabs_.size(); ++i) {
                std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
                float tw = tabWidthForLabel(label);
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
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 1 && tabDragging_) {
        bool tearOut = e.button.y < (int)(barY - 24.f) || e.button.y > (int)(barY + tabBarH_ + 80.f);
        size_t dragged = tabDragIndex_;
        tabDragging_ = false;
        SDL_CaptureMouse(SDL_FALSE);
        hidePopupWindow();
        if (tearOut) detachTabToNewWindow(dragged);
        return true;
    }
    if (e.type == SDL_MOUSEMOTION) {
        float mx = (float)e.motion.x, my = (float)e.motion.y;
        int ww, wh; SDL_GetWindowSize(window_, &ww, &wh);
        float barY = titlebar_->height();
        float localMenuX = std::max(tabStripInset, (float)ww - allFilesW);
        float localInitialVisibleW = std::max(0.f, localMenuX - tabStripInset);
        float localTotalTabsW = 0.f;
        for (auto& tab : tabs_) {
            std::string label = tab.fileName.empty() ? "untitled" : tab.fileName;
            localTotalTabsW += tabWidthForLabel(label);
        }
        bool localOverflow = localTotalTabsW > localInitialVisibleW;
        float localTabAreaX = tabStripInset + (localOverflow ? scrollBtnW * 2.f : 0.f);
        if (my >= barY && my < barY + tabBarH_ && mx >= localTabAreaX && mx < localMenuX) {
            float tx = localTabAreaX - tabScrollX_;
            tabHoverIndex_ = (size_t)-1;
            for (size_t i = 0; i < tabs_.size(); ++i) {
                std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
                float tw = tabWidthForLabel(label);
                if (mx >= tx && mx < tx + tw) { tabHoverIndex_ = i; break; }
                tx += tw;
            }
        } else { tabHoverIndex_ = (size_t)-1; }
    }
    if (e.type == SDL_MOUSEMOTION && tabContextOpen_) {
        float mx = (float)e.motion.x, my = (float)e.motion.y, itemH = 24.f, popW = tabContextMenuWidth(fontAtlas());
        int itemCount = tabContextIndex_ != activeTab_ ? 14 : 13;
        float popH = 4.f + itemCount * itemH;
        tabContextHover_ = (mx >= tabContextX_ && mx <= tabContextX_ + popW && my >= tabContextY_ && my <= tabContextY_ + popH) ? (int)((my - tabContextY_ - 2.f) / itemH) : -1;
        if (tabContextHover_ == 7 || tabContextHover_ == 9 || tabContextHover_ == 12) tabContextHover_ = -1;
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
            float itemH = 24.f, popW = tabContextMenuWidth(fontAtlas());
            int itemCount = tabContextIndex_ != activeTab_ ? 14 : 13;
            float popH = 4.f + itemCount * itemH;
            if (mx >= tabContextX_ && mx <= tabContextX_ + popW && my >= tabContextY_ && my <= tabContextY_ + popH) {
                int idx = (int)((my - tabContextY_ - 2.f) / itemH);
                if (idx == 0) closeTab(tabContextIndex_);
                else if (idx == 1) { for (size_t i = tabs_.size(); i-- > 0;) if (i != tabContextIndex_) { if (tabs_[i].dirty) closeTab(i); else closeTabNow(i); if (closeConfirmOpen_) break; } }
                else if (idx == 2 && tabContextIndex_ < tabs_.size() - 1) { for (size_t i = tabs_.size(); i-- > tabContextIndex_ + 1;) { if (tabs_[i].dirty) closeTab(i); else closeTabNow(i); if (closeConfirmOpen_) break; } }
                else if (idx == 3 && tabContextIndex_ > 0) { for (size_t i = tabContextIndex_; i-- > 0;) { if (tabs_[i].dirty) closeTab(i); else closeTabNow(i); if (closeConfirmOpen_) break; } }
                else if (idx == 4) { for (size_t i = tabs_.size(); i-- > 0;) if (!tabs_[i].dirty) closeTabNow(i); }
                else if (idx == 5) { for (size_t i = tabs_.size(); i-- > tabContextIndex_ + 1;) if (!tabs_[i].dirty) closeTabNow(i); }
                else if (idx == 6) { for (size_t i = tabs_.size(); i-- > 0;) if (!tabs_[i].filePath.empty() && !fs::exists(tabs_[i].filePath)) closeTabNow(i); }
                else if (idx == 10) newBuffer();
                else if (idx == 11) openFileDialog();
                tabContextOpen_ = false; return true;
            }
            tabContextOpen_ = false;
            return true;
        }
        if (tabDropdownOpen_) {
            float popW = 260.f, itemH = 24.f, popX = tabChevronX_, popY = barY + tabBarH_;
            if (mx >= popX && mx <= popX + popW && my >= popY && my <= popY + tabs_.size() * itemH + 4.f) {
                int idx = (int)((my - popY - 2.f) / itemH);
                if (idx >= 0 && idx < (int)tabs_.size()) {
                    collapseEditorGroupsTo((size_t)idx);
                    switchToTab((size_t)idx);
                }
                tabDropdownOpen_ = false; return true;
            }
            tabDropdownOpen_ = false;
            return true;
        }
        if (my < barY || my >= barY + tabBarH_) return false;
        if (hasOverflow && mx >= scrollAreaX && mx < scrollAreaX + scrollAreaW) {
            int btn = (int)((mx - scrollAreaX) / scrollBtnW);
            if (btn == 0) tabScrollX_ -= 80.f;
            else if (btn == 1) tabScrollX_ += 80.f;
            if (tabScrollX_ < 0.f) tabScrollX_ = 0.f;
            if (tabScrollX_ > maxTabScroll) tabScrollX_ = maxTabScroll;
            return true;
        }
        if (mx >= menuX) {
            tabDropdownOpen_ = !tabDropdownOpen_;
            tabDropdownHover_ = -1;
            return true;
        }
        if (mx < tabAreaX || mx >= menuX) return false;
        float tx = tabAreaX - tabScrollX_;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            std::string label = tabs_[i].fileName.empty() ? "untitled" : tabs_[i].fileName;
            float tw = tabWidthForLabel(label);
            if (mx >= tx && mx < tx + tw) {
                if (e.button.button == 3) { tabContextOpen_ = true; tabContextIndex_ = i; tabContextX_ = mx; tabContextY_ = barY + tabBarH_ + 2.f; tabContextHover_ = -1; return true; }
                float actionLeft = tx + tw - tabActionPadR - tabActionW;
                if (e.button.button == 2 || mx >= actionLeft) { closeTab(i); return true; }
                bool ctrlClick = (SDL_GetModState() & KMOD_CTRL) != 0;
                if (ctrlClick) {
                    addEditorGroupForTab(i);
                } else {
                    collapseEditorGroupsTo(i);
                    switchToTab(i);
                }
                if (!ctrlClick && e.button.button == 1 && mx < actionLeft) { tabDragging_ = true; tabDragIndex_ = i; tabDragStartX_ = mx; SDL_CaptureMouse(SDL_TRUE); }
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
    "Automatic","Plain Text","ActionScript","ActionScript 2.0","AppleScript","ASP","Batch File","C","C#","C++",
    "Clojure","CMake","CSS","D","Dart","Diff","Dockerfile","DTD","Elixir","Erlang","Generic Config",
    "Git Config","Git Formats","Go","GraphQL","Graphviz","Groovy","GScript","GOption","Haskell","HTML",
    "INI","Java","JavaScript","JSON","Kotlin","KSP configuration","LaTeX","Lisp","Lua","Makefile",
    "Markdown","MATLAB","Objective-C","OCaml","Pascal","Perl","PHP","PowerShell","Python","R",
    "Rails","Regular Expression","reStructuredText","Ruby","Rust","Scala","ShellScript","SQL",
    "SublimeRC","Swift","TCL","Textile","TOML","TypeScript","XML","YAML"
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

static float tabContextMenuWidth(FontAtlas& font) {
    float maxTextW = 0.f;
    for (const char* item : kTabContextItems) {
        if (!item || !item[0]) continue;
        maxTextW = std::max(maxTextW, font.measureText(item));
    }
    return std::clamp(maxTextW + 32.f, 260.f, 460.f);
}

void Application::updateCommandPalette() {
    commandPalette_.results.clear();
    std::string q = lowerCopy(commandPalette_.query);
    int count = (int)(sizeof(paletteCommands) / sizeof(paletteCommands[0]));
    for (int i = 0; i < count; ++i) {
        std::string label = lowerCopy(paletteCommands[i].label);
        if (q.empty() || label.find(q) != std::string::npos) commandPalette_.results.push_back(i);
    }
    for (int i = 0; i < (int)pluginCommands_.size(); ++i) {
        std::string label = lowerCopy(pluginCommands_[i].caption);
        std::string name = lowerCopy(pluginCommands_[i].name);
        if (q.empty() || label.find(q) != std::string::npos || name.find(q) != std::string::npos) commandPalette_.results.push_back(count + i);
    }
    if (commandPalette_.selected >= (int)commandPalette_.results.size()) commandPalette_.selected = (int)commandPalette_.results.size() - 1;
    if (commandPalette_.selected < 0) commandPalette_.selected = commandPalette_.results.empty() ? -1 : 0;
    int maxScroll = std::max(0, (int)commandPalette_.results.size() - 12);
    if (commandPalette_.scroll > maxScroll) commandPalette_.scroll = maxScroll;
    if (commandPalette_.selected >= 0 && commandPalette_.selected < commandPalette_.scroll) commandPalette_.scroll = commandPalette_.selected;
    if (commandPalette_.selected >= commandPalette_.scroll + 12) commandPalette_.scroll = commandPalette_.selected - 11;
    if (commandPalette_.scroll < 0) commandPalette_.scroll = 0;
}

void Application::refreshPluginCommands() {
    pluginCommands_.clear();
    auto commands = PluginHost::discoverCommands(PluginHost::discoverScripts(paths_.packagesDir));
    pluginCommands_.reserve(commands.size());
    for (auto& command : commands) pluginCommands_.push_back({command.caption, command.name});
}

void Application::executePluginCommand(int index) {
    if (index < 0 || index >= (int)pluginCommands_.size()) return;
    const std::string& commandName = pluginCommands_[index].name;
    uint32_t now = SDL_GetTicks();
    if (!pendingPluginCommand_.empty() && pendingPluginCommand_ == commandName && now - pendingPluginCommandTicks_ < 5000) {
        consoleBuffer_ += "[plugin-command] ignored duplicate " + commandName + "\n";
        return;
    }
    pendingPluginCommand_ = commandName;
    pendingPluginCommandTicks_ = now;
    PluginHostPaths pluginPaths{paths_.exeDir, paths_.dataDir, paths_.packagesDir, paths_.installedPackagesDir, paths_.cacheDir, paths_.libDir};
    PluginHost::runCommand(pluginPaths, commandName);
}

void Application::executePaletteCommand(int commandIndex) {
    int builtinCount = (int)(sizeof(paletteCommands) / sizeof(paletteCommands[0]));
    if (commandIndex >= builtinCount) { executePluginCommand(commandIndex - builtinCount); return; }
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
    sidebarContextOpen_ = false;
    if (pluginQuickPanel_.active) finishPluginQuickPanel(-1);
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
        if (e.type == SDL_DROPFILE) {
            if (e.drop.file) {
                std::string dropped(e.drop.file);
                SDL_free(e.drop.file);
                try {
                    if (fs::is_directory(dropped)) loadFolder(dropped);
                    else openFile(dropped);
                } catch (...) {
                    openFile(dropped);
                }
            }
            continue;
        }
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
            tabDropdownOpen_ = false; tabContextOpen_ = false; sidebarContextOpen_ = false; statusPopup_ = StatusPopup::None; acActive_ = false; commandPalette_.active = false;
            if (pluginQuickPanel_.active) finishPluginQuickPanel(-1);
            hidePopupWindow();
            continue;
        }
        if (popupWin_ && e.type == SDL_WINDOWEVENT && e.window.windowID == SDL_GetWindowID(popupWin_) && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            SDL_Window* focused = SDL_GetKeyboardFocus();
            if (focused != window_ && focused != popupWin_) {
                titlebar_->closeMenuPopup();
                tabDropdownOpen_ = false; tabContextOpen_ = false; sidebarContextOpen_ = false; statusPopup_ = StatusPopup::None; acActive_ = false; commandPalette_.active = false;
                if (pluginQuickPanel_.active) finishPluginQuickPanel(-1);
                hidePopupWindow();
            }
            continue;
        }
        if (closeConfirmOpen_) {
            int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
            std::string name = closeConfirmIndex_ < tabs_.size() ? tabs_[closeConfirmIndex_].fileName : openFile_;
            CloseConfirmLayout dlg = makeCloseConfirmLayout(fontAtlas(), (float)ww, (float)wh, name);
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) { closeConfirmOpen_ = false; continue; }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
                float mx = (float)e.button.x, my = (float)e.button.y;
                auto inButton = [&](float x, float w) { return mx >= x && mx <= x + w && my >= dlg.by && my <= dlg.by + dlg.bh; };
                if (inButton(dlg.saveX, dlg.saveW)) {
                    size_t idx = closeConfirmIndex_;
                    closeConfirmOpen_ = false;
                    switchToTab(idx);
                    saveFile();
                    if (!dirty_) closeTabNow(activeTab_);
                } else if (inButton(dlg.dontSaveX, dlg.dontSaveW)) {
                    size_t idx = closeConfirmIndex_;
                    closeConfirmOpen_ = false;
                    closeTabNow(idx);
                } else if (inButton(dlg.cancelX, dlg.cancelW) || (mx < dlg.mx || mx > dlg.mx + dlg.mw || my < dlg.my || my > dlg.my + dlg.mh)) closeConfirmOpen_ = false;
                continue;
            }
            continue;
        }
        if (e.type == SDL_KEYDOWN) {
            cursorLastInputTicks_ = SDL_GetTicks();
            auto mod = e.key.keysym.mod;
            if (e.key.keysym.sym == SDLK_f && (mod & KMOD_CTRL) && (mod & KMOD_SHIFT) && !(mod & KMOD_ALT)) {
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
        if (pluginInputPanel_.active && e.type == SDL_KEYDOWN) {
            auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { finishPluginInputPanel(false); continue; }
            if (sym == SDLK_RETURN) { finishPluginInputPanel(true); continue; }
            if (sym == SDLK_BACKSPACE && !pluginInputPanel_.text.empty()) {
                auto it = pluginInputPanel_.text.end(); --it;
                while (it != pluginInputPanel_.text.begin() && (*it & 0xC0) == 0x80) --it;
                pluginInputPanel_.text.erase(it, pluginInputPanel_.text.end());
                continue;
            }
            continue;
        }
        if (pluginInputPanel_.active && e.type == SDL_TEXTINPUT) {
            pluginInputPanel_.text += e.text.text;
            continue;
        }
        if (pluginQuickPanel_.active && e.type == SDL_KEYDOWN) {
            auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { finishPluginQuickPanel(-1); continue; }
            int maxScroll = std::max(0, (int)pluginQuickPanel_.items.size() - 12);
            auto clampQuickPanelScroll = [&]() {
                if (pluginQuickPanel_.selected < pluginQuickPanel_.scroll) pluginQuickPanel_.scroll = pluginQuickPanel_.selected;
                if (pluginQuickPanel_.selected >= pluginQuickPanel_.scroll + 12) pluginQuickPanel_.scroll = pluginQuickPanel_.selected - 11;
                if (pluginQuickPanel_.scroll < 0) pluginQuickPanel_.scroll = 0;
                if (pluginQuickPanel_.scroll > maxScroll) pluginQuickPanel_.scroll = maxScroll;
            };
            if (sym == SDLK_RETURN) { finishPluginQuickPanel(pluginQuickPanel_.selected); continue; }
            if (sym == SDLK_UP) { if (pluginQuickPanel_.selected > 0) --pluginQuickPanel_.selected; clampQuickPanelScroll(); continue; }
            if (sym == SDLK_DOWN) { if (pluginQuickPanel_.selected < (int)pluginQuickPanel_.items.size() - 1) ++pluginQuickPanel_.selected; clampQuickPanelScroll(); continue; }
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
            float ow = std::max(260.f, std::min(560.f, (float)ww - 48.f));
            float ox = ((float)ww - ow) / 2.f, oy = titlebar_->height() + tabBarH_ + 36.f;
            float rowH = 24.f, listY = oy + 38.f;
            int visibleRows = std::min(12, std::max(0, (int)commandPalette_.results.size() - commandPalette_.scroll));
            float panelH = 38.f + visibleRows * rowH + 4.f;
            float listH = visibleRows * rowH;
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
                } else if (mx < ox || mx > ox + ow || my < oy || my > oy + panelH) {
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
                            if (std::string(syntaxLanguages[idx]) == "Automatic") {
                                if (activeTab_ < tabs_.size()) tabs_[activeTab_].manualSyntaxName.clear();
                                detectSyntax();
                            } else {
                                syntax_->setLanguageByName(syntaxLanguages[idx]);
                                if (activeTab_ < tabs_.size()) tabs_[activeTab_].manualSyntaxName = syntaxLanguages[idx];
                            }
                            syntaxLangIndex_ = idx;
                            syntaxDirty_ = true; indentsDirty_ = true;
                            syntax_->parse(textBuffer);
                        }
                    }
                } else {
                    statusPopup_ = StatusPopup::None;
                }
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) continue;
        }
        if (pluginInputPanel_.active &&
            (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION)) {
            float ow = std::max(320.f, std::min(620.f, (float)ww - 48.f));
            float ox = ((float)ww - ow) / 2.f, oy = titlebar_->height() + tabBarH_ + 36.f;
            float panelH = 84.f;
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
                float mx = (float)e.button.x, my = (float)e.button.y;
                if (mx < ox || mx > ox + ow || my < oy || my > oy + panelH) finishPluginInputPanel(false);
                continue;
            }
            continue;
        }
        if (pluginQuickPanel_.active &&
            (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL)) {
            float ow = std::max(260.f, std::min(560.f, (float)ww - 48.f));
            float ox = ((float)ww - ow) / 2.f, oy = titlebar_->height() + tabBarH_ + 36.f;
            float rowH = 28.f, listY = oy + 8.f;
            int visibleRows = std::min(12, std::max(0, (int)pluginQuickPanel_.items.size() - pluginQuickPanel_.scroll));
            float panelH = 16.f + visibleRows * rowH;
            float listH = visibleRows * rowH;
            int maxScroll = std::max(0, (int)pluginQuickPanel_.items.size() - 12);
            if (e.type == SDL_MOUSEMOTION) {
                float mx = (float)e.motion.x, my = (float)e.motion.y;
                if (mx >= ox && mx <= ox + ow && my >= listY && my < listY + listH) {
                    int row = (int)((my - listY) / rowH);
                    int idx = pluginQuickPanel_.scroll + row;
                    if (idx >= 0 && idx < (int)pluginQuickPanel_.items.size()) pluginQuickPanel_.selected = idx;
                }
                continue;
            }
            if (e.type == SDL_MOUSEWHEEL) {
                pluginQuickPanel_.scroll -= e.wheel.y * 3;
                if (pluginQuickPanel_.scroll < 0) pluginQuickPanel_.scroll = 0;
                if (pluginQuickPanel_.scroll > maxScroll) pluginQuickPanel_.scroll = maxScroll;
                continue;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
                float mx = (float)e.button.x, my = (float)e.button.y;
                if (mx >= ox && mx <= ox + ow && my >= listY && my < listY + listH) {
                    int row = (int)((my - listY) / rowH);
                    int idx = pluginQuickPanel_.scroll + row;
                    if (idx >= 0 && idx < (int)pluginQuickPanel_.items.size()) finishPluginQuickPanel(idx);
                } else if (mx < ox || mx > ox + ow || my < oy || my > oy + panelH) {
                    finishPluginQuickPanel(-1);
                }
                continue;
            }
            continue;
        }
        if (consoleOpen_ && e.type == SDL_MOUSEWHEEL) {
            SDL_GL_GetDrawableSize(window_, &ww, &wh);
            float sbH = statusbar_->height();
            float consoleTop = (float)wh - sbH - consoleH_;
            if ((float)mouseY_ >= consoleTop && (float)mouseY_ <= (float)wh - sbH) {
                float lineStep = fontAtlas().lineHeight();
                int visibleLines = std::max(1, (int)((consoleH_ - 8.f) / lineStep));
                int lineCount = 1;
                for (char c : consoleBuffer_) if (c == '\n') ++lineCount;
                float maxBack = (float)std::max(0, lineCount - visibleLines);
                consoleScrollY_ += (float)e.wheel.y * 3.f;
                if (consoleScrollY_ < 0.f) consoleScrollY_ = 0.f;
                if (consoleScrollY_ > maxBack) consoleScrollY_ = maxBack;
                continue;
            }
        }
        if ((goto_.active || commandPalette_.active || (acActive_ && !acItems_.empty())) &&
            (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEWHEEL)) {
            continue;
        }
        if (find_.active && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
            float sbH = statusbar_->height();
            float rowH = 28.f, barH = find_.replaceActive ? rowH * 2.f : rowH;
            float barY = (float)wh - sbH - barH;
            float mx = (float)e.button.x, my = (float)e.button.y;
            if (my >= barY && my < barY + barH) {
                float panelW = (float)ww;
                float fieldH = 24.f;
                float toggleX = 18.f;
                float findLabelX = find_.replaceActive ? 186.f : 154.f;
                float fieldX = findLabelX + 48.f;
                float closeButtonW = 24.f;
                float closeX = panelW - closeButtonW - 8.f;
                auto bw = [&](const char* text) { return std::max(54.f, fontAtlas().measureText(text) + 28.f); };
                float findW = bw("Find");
                float findPrevW = bw("Find Prev");
                float findAllW = bw("Find All");
                float replaceW = bw("Replace");
                float replaceAllW = bw("Replace All");
                float gap = 6.f;
                float replaceLeftW = std::max(findW, findAllW);
                float replaceColumnW = std::max(replaceW, replaceAllW);
                float actionW = find_.replaceActive ? replaceLeftW + gap + replaceColumnW : findW + gap + findPrevW + gap + findAllW;
                float fieldW = std::max(150.f, closeX - fieldX - actionW - 10.f);
                float fy = barY + 2.f;
                float findButtonX = fieldX + fieldW + 8.f;
                auto inRect = [&](float x, float y, float w, float h) { return mx >= x && mx < x + w && my >= y && my < y + h; };
                if (inRect(toggleX, fy, 28.f, fieldH)) { find_.regex = !find_.regex; findAllMatches(); continue; }
                if (inRect(toggleX + 34.f, fy, 28.f, fieldH)) { find_.caseSensitive = !find_.caseSensitive; findAllMatches(); continue; }
                if (inRect(toggleX + 68.f, fy, 28.f, fieldH)) { find_.wholeWord = !find_.wholeWord; findAllMatches(); continue; }
                if (inRect(fieldX, fy, fieldW, fieldH)) { findFocus_ = 0; continue; }
                if (inRect(findButtonX, fy, find_.replaceActive ? replaceLeftW : findW, fieldH)) {
                    if (!find_.matches.empty()) {
                        find_.currentMatch = (find_.currentMatch + 1) % find_.matches.size();
                        selections_[0].anchor = selections_[0].cursor = find_.matches[find_.currentMatch];
                        ensureCursorVisible();
                    }
                    continue;
                }
                if (!find_.replaceActive && inRect(findButtonX + findW + gap, fy, findPrevW, fieldH)) {
                    if (!find_.matches.empty()) {
                        find_.currentMatch = (find_.currentMatch + find_.matches.size() - 1) % find_.matches.size();
                        selections_[0].anchor = selections_[0].cursor = find_.matches[find_.currentMatch];
                        ensureCursorVisible();
                    }
                    continue;
                }
                if (find_.replaceActive && inRect(findButtonX + replaceLeftW + gap, fy, replaceColumnW, fieldH)) { doReplace(); continue; }
                if (!find_.replaceActive && inRect(findButtonX + findW + gap + findPrevW + gap, fy, findAllW, fieldH)) {
                    if (!find_.matches.empty()) {
                        selections_.clear();
                        size_t qLen = std::max<size_t>(1, find_.query.size());
                        for (size_t m : find_.matches) selections_.emplace_back(m, std::min(textBuffer.size(), m + qLen));
                        find_.currentMatch = 0;
                        ensureCursorVisible();
                    }
                    continue;
                }
                if (inRect(closeX, fy, closeButtonW, fieldH)) { find_.active = false; continue; }
                if (find_.replaceActive) {
                    float rfy = barY + rowH + 2.f;
                    if (inRect(toggleX, rfy, 28.f, fieldH)) continue;
                    if (inRect(toggleX + 34.f, rfy, 28.f, fieldH)) continue;
                    if (inRect(fieldX, rfy, fieldW, fieldH)) { findFocus_ = 1; continue; }
                    if (inRect(findButtonX, rfy, replaceLeftW, fieldH)) {
                        if (!find_.matches.empty()) {
                            selections_.clear();
                            size_t qLen = std::max<size_t>(1, find_.query.size());
                            for (size_t m : find_.matches) selections_.emplace_back(m, std::min(textBuffer.size(), m + qLen));
                            find_.currentMatch = 0;
                            ensureCursorVisible();
                        }
                        continue;
                    }
                    if (inRect(findButtonX + replaceLeftW + gap, rfy, replaceColumnW, fieldH)) { doReplaceAll(); continue; }
                }
                continue;
            }
        }
        if (handleSidebarEvent(e, (float)wh, titlebar_->height() + tabBarH_, statusbar_->height())) continue;
        if (e.type == SDL_WINDOWEVENT && (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED)) {
            SDL_GL_GetDrawableSize(window_, &ww, &wh); GLRenderer::resize(ww, wh); titlebar_->layout(ww); render();
        }
#ifdef _WIN32
        else if (e.type == SDL_SYSWMEVENT) {
            auto& msg = e.syswm.msg->msg.win;
            if (msg.msg == WM_COPYDATA) {
                auto* cds = reinterpret_cast<COPYDATASTRUCT*>(msg.lParam);
                if (cds && cds->dwData == MorenoTabDropCopyDataId && cds->lpData && cds->cbData > 0) {
                    std::string handoff(static_cast<const char*>(cds->lpData));
                    importDetachedTabFromFile(handoff);
                }
                continue;
            }
            if (msg.msg == WM_SIZING) { SDL_GL_GetDrawableSize(window_, &ww, &wh); GLRenderer::resize(ww, wh); titlebar_->layout(ww); render(); }
        }
#endif
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
            bool drawMinimap = minimapVisible_;
            float mmX = (float)ww - (drawMinimap ? minimap_->width() : 0.f);
            float sbX = mmX - 10.f;
            scrollbarHovered_ = !drawMinimap && e.motion.x >= (int)sbX && e.motion.x < (int)mmX && e.motion.y >= (int)tbH && e.motion.y < wh - (int)(sbH + findPanelH);
            auto clampScroll = [&] { if (scrollY_ < 0.f) scrollY_ = 0.f; if (scrollY_ > maxScroll) scrollY_ = maxScroll; };
            float editorLeft = (sidebarVisible_ ? sidebarWidth_ : 0.f) + gutter_->width() + 8.f;
            float editorRight = mmX - (!drawMinimap ? 10.f : 0.f);
            float editorW = editorRight - editorLeft;
            computeMaxLineWidth();
            float maxScrollX = (!wordWrap_ && editorW > 0.f) ? std::max(0.f, maxLineWidth_ - editorW) : 0.f;
            bool hVisible = maxScrollX > 0.f;
            horizontalScrollbarHovered_ = hVisible && e.motion.x >= (int)editorLeft && e.motion.x < (int)editorRight && e.motion.y >= wh - (int)(sbH + findPanelH) - 12 && e.motion.y < wh - (int)(sbH + findPanelH);
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
                float mmW = (minimapVisible_) ? minimap_->width() : 0.f;
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
            float sbH = statusbar_->height();
            float findPanelH = find_.active ? (find_.replaceActive ? 62.f : 30.f) : 0.f;
            float fww = (float)ww, fwh = (float)wh;
            normalizeEditorGroups();
            size_t groupCount = std::max<size_t>(1, editorGroups_.size());
            float groupAreaLeft = sidebarOffset;
            float groupAreaW = std::max(1.f, fww - groupAreaLeft);
            float groupW = groupCount > 1 ? groupAreaW / (float)groupCount : groupAreaW;
            if (groupCount > 1 && my >= tbH && my < fwh - sbH - findPanelH && mx >= groupAreaLeft) {
                size_t clickedGroup = std::min(groupCount - 1, (size_t)((mx - groupAreaLeft) / groupW));
                if (clickedGroup != activeGroup_ && clickedGroup < editorGroups_.size()) {
                    saveCurrentTab();
                    activeGroup_ = clickedGroup;
                    loadTab(editorGroups_[clickedGroup].tab);
                    continue;
                }
            }
            float activePaneLeft = groupAreaLeft + groupW * (float)activeGroup_;
            float activePaneRight = activeGroup_ + 1 == groupCount ? fww : activePaneLeft + groupW;
            bool drawMinimap = minimapVisible_ && groupW > 260.f;
            float mmW = drawMinimap ? std::min(minimap_->width(), std::max(0.f, groupW * 0.18f)) : 0.f;
            float mmX = activePaneRight - mmW;
            float sbX = mmX - 10.f;
            float gutterW = activePaneLeft + gutter_->width();
            float textX = gutterW + 8.0f;
            float viewH = fwh - tbH - sbH - findPanelH;
            float contentH = totalLines() * lineStep;
            float scrollPad = scrollPastEnd_ ? lineStep * 5.f : 0.f;
            float maxScroll = contentH + scrollPad > viewH ? contentH + scrollPad - viewH : 0.f;
            auto clampScroll = [&] { if (scrollY_ < 0.f) scrollY_ = 0.f; if (scrollY_ > maxScroll) scrollY_ = maxScroll; };
            if (!drawMinimap && mx >= sbX && mx < mmX && my >= tbH && my < fwh - sbH - findPanelH) {
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
            float editorLeft = activePaneLeft + gutter_->width() + 8.f;
            float editorRight = mmX - (!drawMinimap ? 10.f : 0.f);
            float editorW = editorRight - editorLeft;
            computeMaxLineWidth();
            float maxScrollX = (!wordWrap_ && editorW > 0.f) ? std::max(0.f, maxLineWidth_ - editorW) : 0.f;
            if (maxScrollX > 0.f && mx >= editorLeft && mx < editorRight && my >= fwh - sbH - findPanelH - 12.f && my < fwh - sbH - findPanelH) {
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
            if (drawMinimap && mx >= mmX && my >= tbH && my < fwh - sbH - findPanelH) {
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
                                if (std::string(syntaxLanguages[idx]) == "Automatic") {
                                    if (activeTab_ < tabs_.size()) tabs_[activeTab_].manualSyntaxName.clear();
                                    detectSyntax();
                                } else {
                                    syntax_->setLanguageByName(syntaxLanguages[idx]);
                                    if (activeTab_ < tabs_.size()) tabs_[activeTab_].manualSyntaxName = syntaxLanguages[idx];
                                }
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
                float er = fww - ((minimapVisible_) ? 100.f : 0.f) - 10.f;
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
            else if (sym == SDLK_ESCAPE) { if (consoleOpen_) { consoleOpen_ = false; } else { selections_.clear(); selections_.emplace_back(sel.cursor); find_.active = false; goto_.active = false; commandPalette_.active = false; if (pluginInputPanel_.active) finishPluginInputPanel(false); if (pluginQuickPanel_.active) finishPluginQuickPanel(-1); tabDropdownOpen_ = false; acActive_ = false; } }
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
            else if (ctrl && sym == SDLK_BACKQUOTE) { consoleOpen_ = !consoleOpen_; }
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
            else if (sym == SDLK_RETURN) {
                if (acActive_) acceptAutocomplete();
                else if (sendPluginViewTextCommand("insert", "\n")) {}
                else insertText("\n");
            }
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
            if (dirty_) { syntaxDirty_ = true; indentsDirty_ = true; maxLineWidthDirty_ = true; }
            ensureCursorVisible();
        }
        else if (e.type == SDL_TEXTINPUT) { handleAutoPair(e.text.text); }
    }
}

void Application::pollConsoleLog() {
    try {
        fs::path logPath = fs::path(paths_.localDir) / "Console.log";
        if (!fs::exists(logPath)) return;
        uintmax_t size = fs::file_size(logPath);
        if (size < consoleLogReadPos_) consoleLogReadPos_ = 0;
        if (size == consoleLogReadPos_) return;
        std::ifstream f(logPath, std::ios::binary);
        if (!f) return;
        f.seekg((std::streamoff)consoleLogReadPos_);
        std::ostringstream ss;
        ss << f.rdbuf();
        consoleBuffer_ += ss.str();
        consoleLogReadPos_ = (size_t)size;
        constexpr size_t maxConsoleBytes = 256 * 1024;
        if (consoleBuffer_.size() > maxConsoleBytes) {
            size_t eraseTo = consoleBuffer_.size() - maxConsoleBytes;
            size_t nl = consoleBuffer_.find('\n', eraseTo);
            consoleBuffer_.erase(0, nl == std::string::npos ? eraseTo : nl + 1);
        }
    } catch (...) {}
}

void Application::toggleConsole() {
    consoleOpen_ = !consoleOpen_;
    if (consoleOpen_) pollConsoleLog();
}

void Application::pollPluginBridge() {
    if (pluginQuickPanel_.active || pluginInputPanel_.active) return;
    try {
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        if (!fs::exists(bridge)) return;
        fs::path chosen;
        fs::file_time_type chosenTime{};
        for (auto& entry : fs::directory_iterator(bridge)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
            std::string path = entry.path().string();
            if (path.find(".request.json") == std::string::npos || handledPluginRequests_.count(path)) continue;
            auto t = fs::last_write_time(entry.path());
            if (chosen.empty() || t < chosenTime) { chosen = entry.path(); chosenTime = t; }
        }
        if (chosen.empty()) return;
        std::ifstream f(chosen, std::ios::binary);
        if (!f) return;
        nlohmann::json j; f >> j;
        std::string type = j.value("type", "");
        if (type == "view_update") {
            int id = j.value("id", 0);
            if (closedPluginViewIds_.count(id)) {
                handledPluginRequests_.insert(chosen.string());
                return;
            }
            std::string marker = "plugin://view/" + std::to_string(id);
            std::string name = j.value("name", "RC Chat");
            std::string content = j.value("content", "");
            bool focus = j.value("focus", true);
            bool rcChatView = false;
            std::string pluginSyntax;
            std::string pluginColorScheme;
            if (j.contains("settings") && j["settings"].is_object()) {
                rcChatView = j["settings"].value("rc_chat_view", false);
                pluginSyntax = j["settings"].value("syntax", "");
                pluginColorScheme = j["settings"].value("color_scheme", "");
            }
            if (rcChatView && (name.empty() || name == "untitled") && content == "> ") {
                handledPluginRequests_.insert(chosen.string());
                return;
            }
            size_t tabIndex = tabs_.size();
            for (size_t i = 0; i < tabs_.size(); ++i) {
                if (tabs_[i].filePath == marker) { tabIndex = i; break; }
            }
            std::string displayName = name.empty() ? "RC Chat" : name;
            if (tabIndex != tabs_.size() && rcChatView && displayName == "RC Chat" &&
                !tabs_[tabIndex].fileName.empty() && tabs_[tabIndex].fileName != "untitled") {
                displayName = tabs_[tabIndex].fileName;
            }
            if (tabIndex == tabs_.size()) {
                saveCurrentTab();
                TabBuffer tab;
                tab.filePath = marker;
                tab.fileName = displayName;
                tab.text = content;
                tab.dirty = false;
                tab.pluginOwned = true;
                tab.pluginSyntax = pluginSyntax;
                tab.pluginColorScheme = pluginColorScheme;
                tab.selections.emplace_back(content.size());
                tabs_.push_back(std::move(tab));
                tabIndex = tabs_.size() - 1;
            } else {
                tabs_[tabIndex].fileName = displayName;
                tabs_[tabIndex].text = content;
                tabs_[tabIndex].dirty = false;
                if (!pluginSyntax.empty()) tabs_[tabIndex].pluginSyntax = pluginSyntax;
                if (!pluginColorScheme.empty()) tabs_[tabIndex].pluginColorScheme = pluginColorScheme;
                tabs_[tabIndex].selections.clear();
                tabs_[tabIndex].selections.emplace_back(content.size());
            }
            if (focus) {
                if (tabIndex == activeTab_) loadTab(tabIndex);
                else switchToTab(tabIndex);
                if (rcChatView) {
                    ensureCursorVisible();
                    if (activeTab_ < tabs_.size()) tabs_[activeTab_].scrollY = scrollY_;
                }
            }
            else if (tabIndex == activeTab_) {
                loadTab(tabIndex);
                if (rcChatView) {
                    ensureCursorVisible();
                    if (activeTab_ < tabs_.size()) tabs_[activeTab_].scrollY = scrollY_;
                }
            }
            consoleBuffer_ += "[plugin-view] " + displayName + "\n";
            handledPluginRequests_.insert(chosen.string());
            return;
        }
        if (type == "input_panel") {
            pendingPluginCommand_.clear();
            pendingPluginCommandTicks_ = 0;
            pluginInputPanel_.requestPath = chosen.string();
            pluginInputPanel_.responsePath = j.value("response", "");
            pluginInputPanel_.caption = j.value("caption", "");
            pluginInputPanel_.text = j.value("initial_text", "");
            pluginInputPanel_.submitted = false;
            pluginInputPanel_.active = !pluginInputPanel_.responsePath.empty();
            if (pluginInputPanel_.active) {
                consoleBuffer_ += "[plugin-input-panel] " + pluginInputPanel_.caption + "\n";
                hidePopupWindow();
            }
            handledPluginRequests_.insert(chosen.string());
            return;
        }
        if (type != "quick_panel") { handledPluginRequests_.insert(chosen.string()); return; }
        pendingPluginCommand_.clear();
        pendingPluginCommandTicks_ = 0;
        pluginQuickPanel_.items.clear();
        for (auto& item : j["items"]) pluginQuickPanel_.items.push_back(item.get<std::string>());
        pluginQuickPanel_.requestPath = chosen.string();
        pluginQuickPanel_.responsePath = j.value("response", "");
        pluginQuickPanel_.selected = pluginQuickPanel_.items.empty() ? -1 : 0;
        pluginQuickPanel_.scroll = 0;
        pluginQuickPanel_.submitted = false;
        pluginQuickPanel_.active = !pluginQuickPanel_.items.empty() && !pluginQuickPanel_.responsePath.empty();
        if (pluginQuickPanel_.active) {
            consoleBuffer_ += "[plugin-quick-panel] " + std::to_string(pluginQuickPanel_.items.size()) + " items\n";
            hidePopupWindow();
        }
        handledPluginRequests_.insert(chosen.string());
    } catch (...) {}
}

void Application::finishPluginQuickPanel(int index) {
    if (!pluginQuickPanel_.active) return;
    if (pluginQuickPanel_.submitted) return;
    pluginQuickPanel_.submitted = true;
    std::string responsePath = pluginQuickPanel_.responsePath;
    pluginQuickPanel_.active = false;
    hidePopupWindow();
    try {
        nlohmann::json j;
        j["index"] = index;
        fs::path response = responsePath;
        fs::create_directories(response.parent_path());
        std::ofstream f(response, std::ios::binary | std::ios::trunc);
        f << j.dump();
        f.close();
        consoleBuffer_ += "[plugin-quick-panel] selected " + std::to_string(index) + "\n";
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        if (fs::exists(bridge)) {
            for (auto& entry : fs::directory_iterator(bridge)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                std::string path = entry.path().string();
                if (path.find(".request.json") == std::string::npos) continue;
                if (handledPluginRequests_.count(path)) continue;
                try {
                    std::ifstream requestFile(entry.path(), std::ios::binary);
                    nlohmann::json requestJson;
                    requestFile >> requestJson;
                    if (requestJson.value("type", "") == "quick_panel") handledPluginRequests_.insert(path);
                } catch (...) {}
            }
        }
    } catch (...) {}
    pluginQuickPanel_ = {};
}

void Application::finishPluginInputPanel(bool accepted) {
    if (!pluginInputPanel_.active) return;
    if (pluginInputPanel_.submitted) return;
    pluginInputPanel_.submitted = true;
    std::string responsePath = pluginInputPanel_.responsePath;
    std::string text = pluginInputPanel_.text;
    pluginInputPanel_.active = false;
    hidePopupWindow();
    try {
        nlohmann::json j;
        j["accepted"] = accepted;
        j["text"] = accepted ? text : "";
        fs::path response = responsePath;
        fs::create_directories(response.parent_path());
        std::ofstream f(response, std::ios::binary | std::ios::trunc);
        f << j.dump();
        f.close();
        consoleBuffer_ += accepted ? "[plugin-input-panel] accepted\n" : "[plugin-input-panel] canceled\n";
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        if (fs::exists(bridge)) {
            for (auto& entry : fs::directory_iterator(bridge)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                std::string path = entry.path().string();
                if (path.find(".request.json") == std::string::npos) continue;
                if (handledPluginRequests_.count(path)) continue;
                try {
                    std::ifstream requestFile(entry.path(), std::ios::binary);
                    nlohmann::json requestJson;
                    requestFile >> requestJson;
                    if (requestJson.value("type", "") == "input_panel") handledPluginRequests_.insert(path);
                } catch (...) {}
            }
        }
    } catch (...) {}
    pluginInputPanel_ = {};
}

bool Application::writePluginViewCommandRequest(const std::string& type, const std::string& commandName, const std::string& characters) {
    if (openFilePath_.rfind("plugin://view/", 0) != 0) return false;
    std::string idText = openFilePath_.substr(std::string("plugin://view/").size());
    int id = 0;
    try { id = std::stoi(idText); } catch (...) { return false; }
    try {
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        nlohmann::json j;
        j["type"] = type;
        j["id"] = id;
        j["command"] = commandName;
        j["args"] = nlohmann::json::object();
        if (type == "view_text_command") j["args"]["characters"] = characters;
        j["content"] = textBuffer;
        j["cursor"] = selections_.empty() ? textBuffer.size() : selections_[0].cursor;
        fs::path request = bridge / (std::to_string(SDL_GetTicks()) + "-" + std::to_string(id) + "-" + type + ".request.json");
        std::ofstream f(request, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << j.dump();
        consoleBuffer_ += "[" + type + "] " + commandName + "\n";
        return true;
    } catch (...) {
        return false;
    }
}

bool Application::sendPluginViewTextCommand(const std::string& commandName, const std::string& characters) {
    return writePluginViewCommandRequest("view_text_command", commandName, characters);
}

bool Application::sendPluginViewWindowCommand(const std::string& commandName) {
    return writePluginViewCommandRequest("view_window_command", commandName, "");
}

void Application::notifyPluginViewClosed(int id) {
    if (id <= 0) return;
    try {
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        nlohmann::json j;
        j["type"] = "view_closed";
        j["id"] = id;
        fs::path request = bridge / (std::to_string(SDL_GetTicks()) + "-" + std::to_string(id) + "-view_closed.request.json");
        std::ofstream f(request, std::ios::binary | std::ios::trunc);
        if (f) f << j.dump();
    } catch (...) {}
}

void Application::update() {
    pollConsoleLog();
    pollPluginBridge();
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
    normalizeEditorGroups();
    float sidebarOffset = sidebarVisible_ ? sidebarWidth_ : 0.f;
    size_t groupCount = std::max<size_t>(1, editorGroups_.size());
    if (activeGroup_ >= groupCount) activeGroup_ = groupCount - 1;
    float groupAreaLeft = sidebarOffset;
    float groupAreaW = std::max(1.f, fww - groupAreaLeft);
    float groupW = groupCount > 1 ? groupAreaW / (float)groupCount : groupAreaW;
    float activePaneLeft = groupAreaLeft + groupW * (float)activeGroup_;
    float activePaneRight = activeGroup_ + 1 == groupCount ? fww : activePaneLeft + groupW;
    bool drawMinimap = minimapVisible_ && groupW > 260.f;
    float mmW = drawMinimap ? std::min(minimap_->width(), std::max(0.f, groupW * 0.18f)) : 0.f;
    float scrollbarW = drawMinimap ? 0.f : 10.f;
    float tbH = titlebar_->height() + tabBarH_;
    float sbH = statusbar_->height();
    float findPanelH = find_.active ? (find_.replaceActive ? 62.f : 30.f) : 0.f;
    float lineStep = fontAtlas().lineHeight();
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    size_t lineCount = totalLines();
    size_t firstVisibleLine = (size_t)(scrollY_ / lineStep);
    ensureLargeFileWindowForLine(firstVisibleLine);
    lineCount = totalLines();
    size_t currentLine = lineOfPos(selections_[0].cursor);
    size_t currentCol = colOfPos(selections_[0].cursor);
    size_t firstRenderLine = firstVisibleLine > 2 ? firstVisibleLine - 2 : 0;
    size_t lastRenderLine = lineCount > 0 ? std::min(lineCount - 1, (size_t)((scrollY_ + (fwh - sbH - findPanelH - tbH)) / lineStep) + 3) : 0;
    float firstVisibleY = textOriginY + firstVisibleLine * lineStep - scrollY_;
    bool externalPopupWindow = useExternalPopupWindow();
    deferPopupDraw_ = externalPopupWindow && (titlebar_->isMenuOpen() || tabDropdownOpen_ || tabContextOpen_ || sidebarContextOpen_ ||
        statusPopup_ != StatusPopup::None ||
        (acActive_ && !acItems_.empty()));
    if (activeTab_ < tabs_.size()) {
        tabs_[activeTab_].fileName = openFile_.empty() ? "untitled" : openFile_;
        tabs_[activeTab_].filePath = openFilePath_;
        tabs_[activeTab_].dirty = dirty_;
    }

    drawTabBar(fontAtlas(), fww, titlebar_->height());
    drawSidebar(fontAtlas(), fwh, tbH, sbH);

    float gutterW = gutter_->width();
    static std::unordered_map<std::string, std::unique_ptr<SyntaxHighlighter>> previewSyntaxCache;
    auto previewSyntaxForTab = [&](const TabBuffer& tab) -> SyntaxHighlighter& {
        std::string key;
        if (!tab.pluginSyntax.empty()) key = "plugin|" + tab.pluginSyntax + "|" + tab.pluginColorScheme;
        else {
            std::string path = !tab.filePath.empty() ? tab.filePath : tab.fileName;
            std::string ext = fs::path(path).extension().string();
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            key = "ext|" + ext;
        }
        auto it = previewSyntaxCache.find(key);
        if (it != previewSyntaxCache.end()) return *it->second;
        auto highlighter = std::make_unique<SyntaxHighlighter>();
        if (!tab.pluginSyntax.empty()) highlighter->setPluginSyntax(tab.pluginSyntax, tab.pluginColorScheme);
        else {
            std::string path = !tab.filePath.empty() ? tab.filePath : tab.fileName;
            std::string ext = fs::path(path).extension().string();
            if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
            highlighter->setLanguage(ext);
        }
        auto& ref = *highlighter;
        previewSyntaxCache.emplace(std::move(key), std::move(highlighter));
        return ref;
    };
    for (size_t gi = 0; gi < groupCount; ++gi) {
        if (gi == activeGroup_) continue;
        size_t tabIndex = editorGroups_[gi].tab;
        if (tabIndex >= tabs_.size()) continue;
        float paneLeft = groupAreaLeft + groupW * (float)gi;
        float paneRight = gi + 1 == groupCount ? fww : paneLeft + groupW;
        const auto& tab = tabs_[tabIndex];
        auto& paneSyntax = previewSyntaxForTab(tab);
        auto bg = paneSyntax.backgroundColor();
        auto gutterBg = paneSyntax.gutterColor();
        addSolid(paneLeft, tbH, paneRight, fwh - sbH - findPanelH, bg.r, bg.g, bg.b, 1.f);
        addSolid(paneLeft, tbH, paneLeft + gutterW, fwh - sbH - findPanelH, gutterBg.r, gutterBg.g, gutterBg.b, 1.f);
        addSolid(paneRight - 1.f, tbH, paneRight, fwh - sbH, 0.35f, 0.37f, 0.43f, 1.f);
        flushSolid();
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)paneLeft, (int)sbH, (int)std::max(1.f, paneRight - paneLeft - 1.f), wh - (int)(tbH + sbH));
        const std::string& preview = tab.text;
        float yPreview = tbH + fontAtlas().ascent() + 4.f - tab.scrollY;
        size_t line = 0, pos = 0;
        size_t maxLines = 0;
        while (pos <= preview.size() && yPreview < fwh - sbH) {
            size_t end = preview.find('\n', pos);
            if (end == std::string::npos) end = preview.size();
            if (yPreview + lineStep >= tbH) {
                std::string lineNo = std::to_string(line + 1);
                float lw = fontAtlas().measureText(lineNo);
                fontAtlas().drawText(lineNo, paneLeft + gutterW - lw - 8.f, yPreview, 0.45f, 0.45f, 0.50f, 1.f);
                std::string_view text(preview.data() + pos, end - pos);
                auto tokens = paneSyntax.highlightLine(text, pos);
                float cx = paneLeft + gutterW + 8.f;
                float clipLeft = paneLeft + gutterW + 8.f;
                float clipRight = paneRight - 8.f;
                if (tokens.empty()) {
                    auto& c = paneSyntax.scopeColor(0);
                    fontAtlas().drawTextClipped(text, cx, yPreview, clipLeft, clipRight, c.r, c.g, c.b, 1.f);
                } else {
                    size_t prevEnd = 0;
                    for (auto& tok : tokens) {
                        if (tok.start > pos + prevEnd) {
                            auto& c = paneSyntax.scopeColor(0);
                            std::string_view gap(preview.data() + pos + prevEnd, tok.start - pos - prevEnd);
                            fontAtlas().drawTextClipped(gap, cx, yPreview, clipLeft, clipRight, c.r, c.g, c.b, 1.f);
                            cx += fontAtlas().measureText(gap);
                        }
                        auto& c = paneSyntax.scopeColor(tok.scope);
                        std::string_view tokText(preview.data() + tok.start, tok.length);
                        fontAtlas().drawTextClipped(tokText, cx, yPreview, clipLeft, clipRight, c.r, c.g, c.b, 1.f);
                        cx += fontAtlas().measureText(tokText);
                        prevEnd = (tok.start - pos) + tok.length;
                        if (cx > clipRight) break;
                    }
                    if (prevEnd < end - pos) {
                        auto& c = paneSyntax.scopeColor(0);
                        std::string_view rest(preview.data() + pos + prevEnd, end - pos - prevEnd);
                        fontAtlas().drawTextClipped(rest, cx, yPreview, clipLeft, clipRight, c.r, c.g, c.b, 1.f);
                    }
                }
            }
            if (end >= preview.size()) break;
            pos = end + 1;
            ++line;
            ++maxLines;
            yPreview += lineStep;
            if (maxLines > 2000) break;
        }
        glDisable(GL_SCISSOR_TEST);
    }

    float gutterRight = activePaneLeft + gutterW;
    float textX = gutterRight + 8.0f;
    float editorRight = activePaneRight - mmW - scrollbarW;
    float editorWidth = editorRight - textX;
    computeMaxLineWidth();
    float maxScrollX = (!wordWrap_ && editorWidth > 0.f) ? std::max(0.f, maxLineWidth_ - editorWidth) : 0.f;
    if (scrollX_ > maxScrollX) scrollX_ = maxScrollX;
    if (scrollX_ < 0.f || wordWrap_) scrollX_ = 0.f;
    bool horizontalScrollbarVisible = !wordWrap_ && maxScrollX > 0.f;
    float hScrollbarH = horizontalScrollbarVisible ? 12.f : 0.f;
    float editorBottom = fwh - sbH - findPanelH - hScrollbarH;
    float drawTextX = textX - scrollX_;
    auto editorBg = syntax_->backgroundColor();
    addSolid(activePaneLeft, tbH, editorRight, editorBottom, editorBg.r, editorBg.g, editorBg.b, 1.f);
    flushSolid();
    gutter_->draw(fontAtlas(), lineCount, currentLine, firstVisibleLine, activePaneLeft, firstVisibleY, lineStep, fwh - sbH, tbH);

    glEnable(GL_SCISSOR_TEST);
    glScissor((int)gutterRight, (int)(sbH + hScrollbarH), (int)std::max(1.f, editorRight - gutterRight), wh - (int)(tbH + sbH + hScrollbarH));

    {
        float cy = textOriginY + currentLine * lineStep - scrollY_;
        if (cy + lineStep >= tbH && cy <= editorBottom) {
            auto lineBg = syntax_->lineHighlightColor();
            addSolid(gutterRight, cy, editorRight, cy + lineStep, lineBg.r, lineBg.g, lineBg.b, 1.f);
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
                fontAtlas().drawTextClipped(line, drawTextX, y, textX, editorRight, c.r, c.g, c.b, 1.0f);
            } else {
                float cx = drawTextX;
                size_t prevEnd = 0;
                for (auto& tok : tokens) {
                    if (tok.start > lStart + prevEnd) {
                        auto& c = syntax_->scopeColor(0);
                        std::string_view gap(textBuffer.data() + lStart + prevEnd, tok.start - lStart - prevEnd);
                        fontAtlas().drawTextClipped(gap, cx, y, textX, editorRight, c.r, c.g, c.b, 1.0f);
                        cx += fontAtlas().measureText(gap);
                    }
                    auto& c = syntax_->scopeColor(tok.scope);
                    std::string_view tokText(textBuffer.data() + tok.start, tok.length);
                    fontAtlas().drawTextClipped(tokText, cx, y, textX, editorRight, c.r, c.g, c.b, 1.0f);
                    cx += fontAtlas().measureText(tokText);
                    prevEnd = (tok.start - lStart) + tok.length;
                    if (cx > editorRight) break;
                }
                if (prevEnd < lEnd - lStart) {
                    auto& c = syntax_->scopeColor(0);
                    std::string_view rest(textBuffer.data() + lStart + prevEnd, lEnd - lStart - prevEnd);
                    fontAtlas().drawTextClipped(rest, cx, y, textX, editorRight, c.r, c.g, c.b, 1.0f);
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
        addSolid(cx, ct, cx + 2.5f, cb, 0.82f, 0.98f, 1.0f, 1.f);
    }
    flushSolid();
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
    if (!drawMinimap) {
        float sbX = activePaneRight - mmW - scrollbarW;
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
        addSolid(textX, trackY, textX + regionW, trackY + hScrollbarH, 0.105f, 0.105f, 0.125f, 1.f);
        addSolid(thumbX, trackY + 3.f, thumbX + thumbW, trackY + 9.f, c + 0.08f, c + 0.08f, c + 0.08f, 1.f);
        flushSolid();
    }
    // minimap
    if (drawMinimap) {
        float mmX = activePaneRight - mmW;
        bool mmOver = mouseX_ >= (int)mmX && mouseX_ < (int)activePaneRight && mouseY_ >= (int)tbH && mouseY_ < (int)(fwh - sbH - findPanelH);
        minimap_->setMouseOver(mmOver);
        minimap_->updateHoverFade(1.f/60.f);
        float mmContentH = lineCount * lineStep;
        float mmScrollPad = scrollPastEnd_ ? lineStep * 5.f : 0.f;
        float mmViewH = fwh - tbH - sbH - findPanelH;
        float mmMaxScroll = (mmContentH + mmScrollPad > mmViewH) ? mmContentH + mmScrollPad - mmViewH : 0.f;
        minimap_->draw(fontAtlas(), *syntax_, textBuffer, mmX, textOriginY, fwh - findPanelH, tbH, gutterW, lineStep, scrollY_, mmMaxScroll, mmOver);
    }
    // status bar
    std::string branch;
    if (consoleOpen_) {
        float consoleY = fwh - sbH - consoleH_;
        addSolid(0, consoleY, fww, consoleY + consoleH_, 0.08f, 0.08f, 0.10f, 0.95f);
        addSolid(0, consoleY, fww, consoleY + 1, 0.30f, 0.30f, 0.35f, 1.f);
        addSolid(0, consoleY + consoleH_ - 2, fww, consoleY + consoleH_, 0.325f, 0.545f, 1.f, 0.6f);
        flushSolid();
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, wh - (int)(consoleY + consoleH_), ww, (int)(consoleH_));
        float cy = consoleY + 4.f;
        float lineStep = fontAtlas().lineHeight();
        std::string_view buf(consoleBuffer_);
        int visibleLines = std::max(1, (int)((consoleH_ - 8.f) / lineStep));
        std::vector<size_t> lineStarts;
        lineStarts.reserve(256);
        lineStarts.push_back(0);
        for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == '\n' && i + 1 < buf.size()) lineStarts.push_back(i + 1);
        }
        int consoleLineCount = (int)lineStarts.size();
        int maxBack = std::max(0, consoleLineCount - visibleLines);
        if (consoleScrollY_ > (float)maxBack) consoleScrollY_ = (float)maxBack;
        if (consoleScrollY_ < 0.f) consoleScrollY_ = 0.f;
        int firstLine = std::max(0, consoleLineCount - visibleLines - (int)consoleScrollY_);
        int lastLine = std::min(consoleLineCount, firstLine + visibleLines);
        for (int li = firstLine; li < lastLine && cy + lineStep < consoleY + consoleH_; ++li) {
            size_t pos = lineStarts[(size_t)li];
            size_t nl = buf.find('\n', pos);
            if (nl == std::string::npos) nl = buf.size();
            std::string_view line(buf.data() + pos, nl - pos);
            fontAtlas().drawText(line, 8.f, cy, 0.7f, 0.72f, 0.75f, 1.f);
            cy += lineStep;
        }
        if (maxBack > 0 && consoleScrollY_ > 0.f) {
            std::string marker = "console +" + std::to_string((int)consoleScrollY_);
            float mw = fontAtlas().measureText(marker);
            fontAtlas().drawText(marker, fww - mw - 14.f, consoleY + 6.f, 0.48f, 0.62f, 0.86f, 1.f);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    { std::lock_guard<std::mutex> lock(gitBranchMutex_); branch = gitBranch_; }
    statusbar_->appendSolidRects(fontAtlas(), solidVerts_, fww, fwh, currentLine, currentCol, branch);
    flushSolid();
    statusbar_->drawText(fontAtlas(), fww, fwh, currentLine, currentCol, syntax_->languageName(), useTabs_, tabSize_, branch);
    // find bar
    if (find_.active) {
        float rowH = 28.f, barH = find_.replaceActive ? rowH * 2.f : rowH;
        float barY = fwh - sbH - barH;
        float barW = fww;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(0, barY, barW, barY + barH, 0.18f, 0.18f, 0.21f, 1.f);
        ar(0, barY, barW, barY + 1, 0.34f, 0.35f, 0.39f, 1.f);
        float fieldH = 24.f;
        float toggleX = 18.f;
        float findLabelX = find_.replaceActive ? 186.f : 154.f;
        float fieldX = findLabelX + 48.f;
        float closeButtonW = 24.f;
        float closeButtonX = barW - closeButtonW - 8.f;
        auto buttonW = [&](const char* text) { return std::max(54.f, fontAtlas().measureText(text) + 28.f); };
        float findW = buttonW("Find");
        float findPrevW = buttonW("Find Prev");
        float findAllW = buttonW("Find All");
        float replaceW = buttonW("Replace");
        float replaceAllW = buttonW("Replace All");
        float replaceLeftW = std::max(findW, findAllW);
        float replaceColumnW = std::max(replaceW, replaceAllW);
        float gap = 6.f;
        float actionW = find_.replaceActive ? replaceLeftW + gap + replaceColumnW : findW + gap + findPrevW + gap + findAllW;
        float fieldW = std::max(150.f, closeButtonX - fieldX - actionW - 10.f);
        float fy = barY + 2.f;
        auto button = [&](float x, float y, float w, bool active = false) {
            ar(x, y, x + w, y + fieldH, active ? 0.28f : 0.24f, active ? 0.31f : 0.26f, active ? 0.38f : 0.30f, 1.f);
        };
        auto centeredText = [&](const char* text, float x, float y, float w) {
            float textW = fontAtlas().measureText(text);
            fontAtlas().drawText(text, x + (w - textW) * 0.5f, y + 5.f, 0.82f, 0.84f, 0.88f, 1.f);
        };
        auto toggleBtn = [&](bool active, float x, float y) {
            button(x, y, 28.f, active);
        };
        toggleBtn(find_.regex, toggleX, fy);
        toggleBtn(find_.caseSensitive, toggleX + 34.f, fy);
        toggleBtn(find_.wholeWord, toggleX + 68.f, fy);
        button(fieldX, fy, fieldW, false);
        float findButtonX = fieldX + fieldW + 8.f;
        button(findButtonX, fy, find_.replaceActive ? replaceLeftW : findW);
        button(findButtonX + (find_.replaceActive ? replaceLeftW : findW) + gap, fy, find_.replaceActive ? replaceColumnW : findPrevW);
        if (!find_.replaceActive) button(findButtonX + findW + gap + findPrevW + gap, fy, findAllW);
        button(closeButtonX, fy, closeButtonW);
        float replaceFieldY = 0.f;
        float replaceButtonX = 0.f;
        float replaceAllButtonX = 0.f;
        if (find_.replaceActive) {
            float rfy = barY + rowH + 2.f;
            replaceFieldY = rfy;
            button(toggleX, rfy, 28.f);
            button(toggleX + 34.f, rfy, 28.f);
            button(fieldX, rfy, fieldW);
            replaceButtonX = findButtonX;
            replaceAllButtonX = replaceButtonX + replaceLeftW + gap;
            button(replaceButtonX, rfy, replaceLeftW);
            button(replaceAllButtonX, rfy, replaceColumnW);
        }
        flushSolid();
        std::string findText = find_.query + (findFocus_ == 0 ? "|" : "");
        fontAtlas().drawText(".*", toggleX + 6.f, fy + 5.f, find_.regex ? 0.95f : 0.58f, find_.regex ? 0.86f : 0.58f, find_.regex ? 0.95f : 0.62f, 1.f);
        fontAtlas().drawText("Aa", toggleX + 39.f, fy + 5.f, find_.caseSensitive ? 0.95f : 0.58f, find_.caseSensitive ? 0.86f : 0.58f, find_.caseSensitive ? 0.95f : 0.62f, 1.f);
        fontAtlas().drawText("\\b", toggleX + 73.f, fy + 5.f, find_.wholeWord ? 0.95f : 0.58f, find_.wholeWord ? 0.86f : 0.58f, find_.wholeWord ? 0.95f : 0.62f, 1.f);
        fontAtlas().drawText("Find:", findLabelX, fy + 5.f, 0.78f, 0.78f, 0.82f, 1.f);
        fontAtlas().drawText(findText, fieldX + 6.f, fy + 4.f, 0.85f, 0.85f, 0.88f, 1.f);
        if (!find_.matches.empty()) {
            std::string mc = std::to_string(find_.matches.size()) + " matches";
            float mcW = fontAtlas().measureText(mc);
            fontAtlas().drawText(mc, fieldX + fieldW - mcW - 8.f, fy + 4.f, 0.5f, 0.5f, 0.55f, 1.f);
        } else if (!find_.query.empty()) {
            float mw = fontAtlas().measureText("No matches");
            fontAtlas().drawText("No matches", fieldX + fieldW - mw - 8.f, fy + 4.f, 0.6f, 0.3f, 0.3f, 1.f);
        }
        centeredText("Find", findButtonX, fy, find_.replaceActive ? replaceLeftW : findW);
        centeredText(find_.replaceActive ? "Replace" : "Find Prev", findButtonX + (find_.replaceActive ? replaceLeftW : findW) + gap, fy, find_.replaceActive ? replaceColumnW : findPrevW);
        if (!find_.replaceActive) centeredText("Find All", findButtonX + findW + gap + findPrevW + gap, fy, findAllW);
        fontAtlas().drawText("\xc3\x97", closeButtonX + 7.f, fy + 4.f, 0.65f, 0.65f, 0.68f, 1.f);
        if (find_.replaceActive) {
            std::string repText = find_.replace + (findFocus_ == 1 ? "|" : "");
            fontAtlas().drawText("AB", toggleX + 4.f, replaceFieldY + 5.f, 0.58f, 0.58f, 0.62f, 1.f);
            fontAtlas().drawText("<>", toggleX + 38.f, replaceFieldY + 5.f, 0.58f, 0.58f, 0.62f, 1.f);
            fontAtlas().drawText("Replace:", findLabelX - 28.f, replaceFieldY + 5.f, 0.78f, 0.78f, 0.82f, 1.f);
            fontAtlas().drawText(repText, fieldX + 6.f, replaceFieldY + 4.f, 0.85f, 0.85f, 0.88f, 1.f);
            centeredText("Find All", replaceButtonX, replaceFieldY, replaceLeftW);
            centeredText("Replace All", replaceAllButtonX, replaceFieldY, replaceColumnW);
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
    if (commandPalette_.active) {
        float ow = std::max(260.f, std::min(560.f, fww - 48.f));
        float headerH = 38.f, rowH = 24.f;
        int total = (int)commandPalette_.results.size();
        int visible = std::min(12, total - commandPalette_.scroll);
        if (visible < 0) visible = 0;
        float oh = headerH + visible * rowH + 4.f;
        float ox = (fww - ow) / 2.f, oy = tbH + 36.f;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(ox, oy, ox + ow, oy + oh, 0.15f, 0.15f, 0.18f, 0.98f);
        ar(ox, oy, ox + ow, oy + headerH, 0.12f, 0.12f, 0.14f, 1.f);
        int selectedRow = commandPalette_.selected - commandPalette_.scroll;
        if (selectedRow >= 0 && selectedRow < visible)
            ar(ox + 2.f, oy + headerH + selectedRow * rowH, ox + ow - 8.f, oy + headerH + (selectedRow + 1) * rowH, 0.25f, 0.30f, 0.45f, 1.f);
        if (total > 12) {
            float trackX = ox + ow - 6.f;
            float listH = 12.f * rowH;
            float thumbH = std::max(20.f, listH * (12.f / (float)total));
            float maxScroll = (float)std::max(1, total - 12);
            float thumbY = oy + headerH + ((float)commandPalette_.scroll / maxScroll) * (listH - thumbH);
            ar(trackX, thumbY, trackX + 4.f, thumbY + thumbH, 0.32f, 0.32f, 0.35f, 1.f);
        }
        flushSolid();
        fontAtlas().drawText(">", ox + 12.f, oy + 9.f, 0.70f, 0.72f, 0.78f, 1.f);
        fontAtlas().drawText("_", ox + 22.f, oy + 12.f, 0.70f, 0.72f, 0.78f, 1.f);
        fontAtlas().drawText(commandPalette_.query + "|", ox + 42.f, oy + 8.f, 0.84f, 0.84f, 0.86f, 1.f);
        std::string count = std::to_string(total) + " commands";
        float countW = fontAtlas().measureText(count);
        fontAtlas().drawText(count, ox + ow - countW - 14.f, oy + 8.f, 0.46f, 0.46f, 0.50f, 1.f);
        int builtinCount = (int)(sizeof(paletteCommands) / sizeof(paletteCommands[0]));
        for (int row = 0; row < visible; ++row) {
            int resultIndex = commandPalette_.scroll + row;
            int idx = commandPalette_.results[resultIndex];
            float b = resultIndex == commandPalette_.selected ? 0.98f : 0.72f;
            const char* shortcut = idx < builtinCount ? paletteCommands[idx].shortcut : "";
            std::string label = idx < builtinCount ? paletteCommands[idx].label : pluginCommands_[idx - builtinCount].caption;
            fontAtlas().drawText(label, ox + 12.f, oy + headerH + 4.f + row * rowH, b, b, b + 0.03f, 1.f);
            if (shortcut[0]) {
                float sw = fontAtlas().measureText(shortcut);
                fontAtlas().drawText(shortcut, ox + ow - sw - 16.f, oy + headerH + 4.f + row * rowH, 0.48f, 0.48f, 0.52f, 1.f);
            }
        }
    }
    if (pluginQuickPanel_.active) {
        float mainW = std::max(260.f, std::min(560.f, fww - 48.f));
        float rowH = 28.f;
        int total = (int)pluginQuickPanel_.items.size();
        int visible = std::min(12, total - pluginQuickPanel_.scroll);
        if (visible < 0) visible = 0;
        float mainH = 16.f + visible * rowH;
        float mainX = (fww - mainW) / 2.f;
        float mainY = titlebar_->height() + tabBarH_ + 36.f;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(mainX, mainY, mainX + mainW, mainY + mainH, 0.15f, 0.15f, 0.18f, 0.98f);
        int selectedRow = pluginQuickPanel_.selected - pluginQuickPanel_.scroll;
        if (selectedRow >= 0 && selectedRow < visible)
            ar(mainX + 2.f, mainY + 8.f + selectedRow * rowH, mainX + mainW - 8.f, mainY + 8.f + (selectedRow + 1) * rowH, 0.25f, 0.30f, 0.45f, 1.f);
        if (total > 12) {
            float trackX = mainX + mainW - 6.f;
            float listH = 12.f * rowH;
            float thumbH = std::max(20.f, listH * (12.f / (float)total));
            float maxScroll = (float)std::max(1, total - 12);
            float thumbY = mainY + 8.f + ((float)pluginQuickPanel_.scroll / maxScroll) * (listH - thumbH);
            ar(trackX, thumbY, trackX + 4.f, thumbY + thumbH, 0.32f, 0.32f, 0.35f, 1.f);
        }
        flushSolid();
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)mainX, wh - (int)(mainY + mainH), (int)mainW, (int)mainH);
        for (int row = 0; row < visible; ++row) {
            int idx = pluginQuickPanel_.scroll + row;
            float b = idx == pluginQuickPanel_.selected ? 0.98f : 0.72f;
            fontAtlas().drawText(pluginQuickPanel_.items[idx], mainX + 12.f, mainY + 13.f + row * rowH, b, b, b + 0.03f, 1.f);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    if (pluginInputPanel_.active) {
        float mainW = std::max(320.f, std::min(620.f, fww - 48.f));
        float mainH = 84.f;
        float mainX = (fww - mainW) / 2.f;
        float mainY = titlebar_->height() + tabBarH_ + 36.f;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            addSolid(x0, y0, x1, y1, r, g, b, a);
        };
        ar(mainX, mainY, mainX + mainW, mainY + mainH, 0.15f, 0.15f, 0.18f, 0.98f);
        ar(mainX + 10.f, mainY + 42.f, mainX + mainW - 10.f, mainY + 68.f, 0.10f, 0.11f, 0.14f, 1.f);
        flushSolid();
        std::string caption = pluginInputPanel_.caption.empty() ? "Input:" : pluginInputPanel_.caption;
        fontAtlas().drawText(caption, mainX + 12.f, mainY + 14.f, 0.88f, 0.88f, 0.90f, 1.f);
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)(mainX + 12.f), wh - (int)(mainY + 68.f), (int)(mainW - 24.f), 26);
        fontAtlas().drawText(pluginInputPanel_.text, mainX + 16.f, mainY + 48.f, 0.94f, 0.94f, 0.96f, 1.f);
        float tw = fontAtlas().measureText(pluginInputPanel_.text);
        uint32_t blink = (SDL_GetTicks() / 500) % 2;
        if (!blink) fontAtlas().drawText("|", mainX + 16.f + tw, mainY + 48.f, accentColor_.r, accentColor_.g, accentColor_.b, 1.f);
        glDisable(GL_SCISSOR_TEST);
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
        std::string name = closeConfirmIndex_ < tabs_.size() ? tabs_[closeConfirmIndex_].fileName : openFile_;
        CloseConfirmLayout dlg = makeCloseConfirmLayout(fontAtlas(), fww, fwh, name);
        std::vector<float> mv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            mv.insert(mv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        ar(dlg.mx, dlg.my, dlg.mx + dlg.mw, dlg.my + dlg.mh, 0.17f, 0.17f, 0.20f, 0.98f);
        ar(dlg.mx, dlg.my, dlg.mx + dlg.mw, dlg.my + 1.f, 0.35f, 0.35f, 0.40f, 1.f);
        ar(dlg.saveX, dlg.by, dlg.saveX + dlg.saveW, dlg.by + dlg.bh, 0.24f, 0.28f, 0.34f, 1.f);
        ar(dlg.dontSaveX, dlg.by, dlg.dontSaveX + dlg.dontSaveW, dlg.by + dlg.bh, 0.24f, 0.28f, 0.34f, 1.f);
        ar(dlg.cancelX, dlg.by, dlg.cancelX + dlg.cancelW, dlg.by + dlg.bh, 0.24f, 0.28f, 0.34f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, mv.size()*sizeof(float), mv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(mv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        std::string prompt = fitText(fontAtlas(), "Save changes to " + name + " before closing?", dlg.mw - 44.f);
        fontAtlas().drawText(prompt, dlg.mx + 22.f, dlg.my + 28.f, 0.86f, 0.86f, 0.88f, 1.f);
        auto drawCenteredButtonText = [&](const std::string& label, float x, float w) {
            float textW = fontAtlas().measureText(label);
            fontAtlas().drawText(label, x + (w - textW) * 0.5f, dlg.by + 6.f, 0.82f, 0.82f, 0.86f, 1.f);
        };
        drawCenteredButtonText("Save", dlg.saveX, dlg.saveW);
        drawCenteredButtonText("Don't Save", dlg.dontSaveX, dlg.dontSaveW);
        drawCenteredButtonText("Cancel", dlg.cancelX, dlg.cancelW);
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
    // render overflow popups to a separate native-shaped window where the platform supports it.
    if (externalPopupWindow && tabDragging_ && tabDragIndex_ < tabs_.size()) {
        std::string label = tabs_[tabDragIndex_].fileName.empty() ? "untitled" : tabs_[tabDragIndex_].fileName;
        float textW = fontAtlas().measureText(label);
        int ghostW = static_cast<int>(std::ceil(std::clamp(textW + 52.f, 124.f, 320.f)));
        int ghostH = static_cast<int>(tabBarH_);
        int globalX = 0, globalY = 0;
        SDL_GetGlobalMouseState(&globalX, &globalY);
        renderPopupToWindow(globalX - ghostW / 2, globalY - ghostH / 2, ghostW, ghostH);
        clearPopupWindowShape();

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

        std::vector<float> gv;
        drawRect(gv, 0.f, 0.f, static_cast<float>(ghostW), static_cast<float>(ghostH), 0.18f, 0.18f, 0.21f, 0.98f);
        drawRect(gv, 0.f, static_cast<float>(ghostH) - 2.f, static_cast<float>(ghostW), static_cast<float>(ghostH), 0.28f, 0.78f, 0.74f, 1.f);
        if (tabs_[tabDragIndex_].dirty)
            drawRect(gv, static_cast<float>(ghostW) - 20.f, 13.f, static_cast<float>(ghostW) - 14.f, 19.f, 0.85f, 0.66f, 0.22f, 1.f);
        flush(gv);
        fontAtlas().drawTextClipped(label, 12.f, 8.f, 10.f, static_cast<float>(ghostW) - 36.f, 0.88f, 0.88f, 0.90f, 1.f);

        GLRenderer::endFrame();
        SDL_GL_SwapWindow(popupWin_);
        SDL_GL_MakeCurrent(window_, glContext_);
        GLRenderer::resize(ww, wh);
        return;
    }
    bool hasPopup = false;
    float mainX = 0.f, mainY = 0.f, mainW = 0.f, mainH = 0.f;
    enum class PopupKind { None, Menu, TabDropdown, TabContext, SidebarContext, Status, Autocomplete } popupKind = PopupKind::None;
    if (titlebar_->isMenuOpen()) {
        titlebar_->getMenuPopupBounds(fontAtlas(), mainX, mainY, mainW, mainH);
        popupKind = PopupKind::Menu; hasPopup = true;
    } else if (tabDropdownOpen_) {
        mainX = tabChevronX_; mainY = titlebar_->height() + tabBarH_;
        mainW = 260.f; mainH = static_cast<float>(tabs_.size()) * 24.f + 4.f;
        popupKind = PopupKind::TabDropdown; hasPopup = true;
    } else if (tabContextOpen_) {
        mainX = tabContextX_; mainY = tabContextY_;
        int itemCount = tabContextIndex_ != activeTab_ ? 14 : 13;
        mainW = tabContextMenuWidth(fontAtlas()); mainH = 4.f + itemCount * 24.f;
        popupKind = PopupKind::TabContext; hasPopup = true;
    } else if (sidebarContextOpen_) {
        mainX = sidebarContextX_; mainY = sidebarContextY_;
        mainW = 260.f; mainH = 4.f + sidebarContextItems_.size() * 24.f;
        popupKind = PopupKind::SidebarContext; hasPopup = true;
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
    } else if (acActive_ && !acItems_.empty()) {
        size_t cur = selections_[0].cursor, ls = lineStart(cur);
        size_t cl = lineOfPos(cur);
        mainX = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, cur-ls));
        mainY = textOriginY + cl * lineStep - scrollY_ + fontAtlas().ascent() - fontAtlas().descent() + 4.f;
        mainW = 220.f; mainH = static_cast<float>(acItems_.size()) * 22.f + 4.f;
        popupKind = PopupKind::Autocomplete; hasPopup = true;
    }
    if (externalPopupWindow && hasPopup && mainW > 1.f && mainH > 1.f) {
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
            std::vector<float> cv; float itemH = 24.f;
            int itemCount = tabContextIndex_ != activeTab_ ? 14 : 13;
            drawRect(cv, 0, 0, mainW, mainH, 0.17f, 0.17f, 0.20f, 0.98f);
            for (int i = 0; i < itemCount; ++i) if (!kTabContextItems[i][0]) drawRect(cv, 8.f, 13.f + i * itemH, mainW - 8.f, 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f);
            if (tabContextHover_ >= 0 && tabContextHover_ < itemCount && kTabContextItems[tabContextHover_][0]) drawRect(cv, 2, 2 + tabContextHover_ * itemH, mainW - 2, 2 + (tabContextHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
            flush(cv);
            for (int i = 0; i < itemCount; ++i) if (kTabContextItems[i][0]) {
                bool greyed = (i == 2 && tabContextIndex_ >= tabs_.size() - 1) || (i == 3 && tabContextIndex_ == 0);
                float a = greyed ? 0.4f : 1.f;
                fontAtlas().drawText(kTabContextItems[i], 10.f, 6.f + i * itemH, 0.78f * a, 0.78f * a, 0.82f * a, a);
            }
        } else if (popupKind == PopupKind::SidebarContext) {
            std::vector<float> cv; float itemH = 24.f;
            drawRect(cv, 0, 0, mainW, mainH, 0.17f, 0.17f, 0.20f, 0.98f);
            for (int i = 0; i < (int)sidebarContextItems_.size(); ++i) {
                if (sidebarContextItems_[i].separator) drawRect(cv, 8.f, 13.f + i * itemH, mainW - 8.f, 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f);
            }
            if (sidebarContextHover_ >= 0 && sidebarContextHover_ < (int)sidebarContextItems_.size())
                drawRect(cv, 2, 2 + sidebarContextHover_ * itemH, mainW - 2, 2 + (sidebarContextHover_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
            flush(cv);
            for (int i = 0; i < (int)sidebarContextItems_.size(); ++i)
                if (!sidebarContextItems_[i].separator) fontAtlas().drawText(sidebarContextItems_[i].label, 10.f, 6.f + i * itemH, 0.78f, 0.78f, 0.82f, 1.f);
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
    if (popupWin_ && (!externalPopupWindow || !hasPopup)) hidePopupWindow();
}

void Application::shutdown() {
    saveSession();
    saveRecentFiles();
    sidebarWatchRunning_ = false;
    if (popupWin_) { SDL_DestroyWindow(popupWin_); popupWin_ = nullptr; }
#ifdef _WIN32
    if (nativeWindowHandle_) RemovePropA(reinterpret_cast<HWND>(nativeWindowHandle_), MorenoWindowPropName);
#endif
    delete syntax_; delete statusbar_; delete minimap_; delete gutter_; delete titlebar_;
    fontAtlas().destroy(); GLRenderer::destroy();
    SDL_StopTextInput(); SDL_GL_DeleteContext(glContext_); SDL_DestroyWindow(window_); SDL_Quit();
}

int main(int argc, char** argv) { return Application::instance().run(argc, argv); }
