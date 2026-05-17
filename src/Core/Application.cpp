#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "UI/Titlebar.h"
#include "UI/MenuBar.h"
#include "UI/Gutter.h"
#include "UI/Minimap.h"
#include "UI/StatusBar.h"
#include "Syntax/SyntaxHighlighter.h"
#include "Platform/Platform.h"
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
    if (selections_[0].hasSelection()) deleteSelection();
    size_t pos = selections_[0].cursor;
    textBuffer.insert(pos, text);
    selections_[0].anchor = selections_[0].cursor = pos + text.size();
    desiredCursorX_ = -1.f; dirty_ = true;
}
void Application::insertAtCursor(const std::string& text) { insertText(text); }
void Application::deleteSelection() {
    if (!selections_[0].hasSelection()) return;
    size_t a = selections_[0].min(), b = selections_[0].max();
    textBuffer.erase(a, b - a);
    selections_[0].anchor = selections_[0].cursor = a;
    dirty_ = true;
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
    if (openFilePath_.empty()) { syntax_->setLanguage(""); return; }
    std::string ext = fs::path(openFilePath_).extension().string();
    if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
    syntax_->setLanguage(ext);
}

// ── file ops ──

void Application::openFile(const std::string& path) {
    saveCurrentTab();
    std::ifstream f(path, std::ios::binary); if (!f) return;
    std::ostringstream ss; ss << f.rdbuf();
    TabBuffer tab; tab.text = ss.str(); tab.filePath = path; tab.fileName = fs::path(path).filename().string();
    tab.selections.emplace_back(tab.text.size());
    tabs_.push_back(std::move(tab)); activeTab_ = tabs_.size()-1;
    textBuffer = tabs_[activeTab_].text; openFilePath_ = path; openFile_ = tabs_[activeTab_].fileName;
    dirty_ = false; selections_.clear(); selections_.emplace_back(textBuffer.size());
    scrollY_ = 0; undoStack_.clear(); redoStack_.clear(); foldedLines_.clear(); detectSyntax();
}
void Application::saveFile() { if (openFilePath_.empty()) { saveFileAs(); return; } std::ofstream f(openFilePath_, std::ios::binary); if (!f) return; f << textBuffer; dirty_ = false; }
void Application::saveFileAs() {
#ifdef _WIN32
    char buf[MAX_PATH] = {}; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "All Files\0*.*\0Text Files\0*.txt\0"; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "txt";
    if (GetSaveFileNameA(&ofn)) { openFilePath_ = buf; openFile_ = fs::path(buf).filename().string(); detectSyntax(); saveFile(); }
#endif
}
void Application::newBuffer() {
    saveCurrentTab();
    TabBuffer tab; tab.fileName = "untitled"; tab.selections.emplace_back(0);
    tabs_.push_back(std::move(tab)); activeTab_ = tabs_.size()-1;
    textBuffer.clear(); openFilePath_.clear(); openFile_ = "untitled"; dirty_ = false;
    selections_.clear(); selections_.emplace_back(0); scrollY_ = 0; undoStack_.clear(); redoStack_.clear(); foldedLines_.clear(); detectSyntax();
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

bool Application::isFolded(size_t line) const { return line < foldedLines_.size() && foldedLines_[line]; }
size_t Application::findFoldEnd(size_t startLine) const {
    size_t ls = lineStartForLine(startLine), le = lineEnd(ls);
    if (le >= textBuffer.size()) return startLine;
    char openCh = textBuffer[le]; // the '{', '[', '(' at end of line
    char closeCh = (openCh == '{') ? '}' : (openCh == '[') ? ']' : (openCh == '(') ? ')' : 0;
    if (!closeCh) return startLine;
    int depth = 1; size_t pos = le + 1;
    while (pos < textBuffer.size() && depth > 0) {
        if (textBuffer[pos] == openCh) ++depth;
        else if (textBuffer[pos] == closeCh) --depth;
        ++pos;
    }
    return lineOfPos(pos);
}
void Application::toggleFold(size_t line) {
    if (foldedLines_.size() <= line) foldedLines_.resize(line + 20, false);
    foldedLines_[line] = !foldedLines_[line];
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

// ── tabs ──

void Application::saveCurrentTab() {
    if (activeTab_ >= tabs_.size()) return;
    auto& tab = tabs_[activeTab_];
    tab.text = textBuffer; tab.filePath = openFilePath_; tab.fileName = openFile_;
    tab.selections = selections_; tab.scrollY = scrollY_;
    tab.undoStack = std::move(undoStack_); tab.redoStack = std::move(redoStack_);
    tab.foldedLines = foldedLines_; tab.dirty = dirty_; tab.desiredCursorX = desiredCursorX_;
}

void Application::loadTab(size_t index) {
    if (index >= tabs_.size()) return;
    auto& tab = tabs_[index];
    textBuffer = tab.text; openFilePath_ = tab.filePath; openFile_ = tab.fileName;
    selections_ = tab.selections; scrollY_ = tab.scrollY;
    undoStack_ = std::move(tab.undoStack); redoStack_ = std::move(tab.redoStack);
    foldedLines_ = tab.foldedLines; dirty_ = tab.dirty; desiredCursorX_ = tab.desiredCursorX;
    activeTab_ = index; detectSyntax();
}

void Application::switchToTab(size_t index) {
    if (index >= tabs_.size() || index == activeTab_) return;
    saveCurrentTab(); loadTab(index);
}

void Application::closeTab(size_t index) {
    if (index >= tabs_.size()) return;
    tabs_.erase(tabs_.begin() + index);
    if (tabs_.empty()) { newBuffer(); return; }
    if (activeTab_ >= tabs_.size()) activeTab_ = tabs_.size()-1;
    loadTab(activeTab_);
}

void Application::drawTabBar(FontAtlas& font, float windowW, float titlebarH) {
    std::vector<float> v;
    auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        v.insert(v.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    float barY = titlebarH;
    ar(0, barY, windowW, barY + tabBarH_, 0.14f, 0.14f, 0.17f, 1.f);
    ar(0, barY + tabBarH_, windowW, barY + tabBarH_ + 1, 0.10f, 0.10f, 0.12f, 1.f);
    float tx = 4.f;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        std::string label = tabs_[i].fileName;
        if (tabs_[i].dirty) label += "\xe2\x80\xa2";
        float tw = font.measureText(label) + 28.f;
        if (i == activeTab_) {
            ar(tx, barY, tx + tw, barY + tabBarH_, 0.18f, 0.18f, 0.21f, 1.f);
            ar(tx, barY + tabBarH_, tx + tw, barY + tabBarH_ + 1, 0.22f, 0.44f, 0.72f, 1.f);
        }
        font.drawText(label, tx + 8.f, barY + 5.f, (i==activeTab_)?0.9f:0.65f, (i==activeTab_)?0.9f:0.65f, (i==activeTab_)?0.95f:0.7f, 1.f);
        font.drawText("\xc3\x97", tx + tw - 16.f, barY + tabBarH_/2.f - 4.f, 0.5f, 0.5f, 0.55f, 1.f);
        tx += tw + 1.f;
    }
    if (!v.empty()) {
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, v.size()*sizeof(float), v.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size()/8));
        glBindVertexArray(0); GLRenderer::setDrawMode(0);
    }
}

bool Application::handleTabBarEvent(const SDL_Event& e, float windowW, float titlebarH) {
    float barY = titlebarH;
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        float mx = (float)e.button.x, my = (float)e.button.y;
        if (my < barY || my >= barY + tabBarH_) return false;
        float tx = 4.f;
        for (size_t i = 0; i < tabs_.size(); ++i) {
            std::string label = tabs_[i].fileName;
            if (tabs_[i].dirty) label += "\xe2\x80\xa2";
            float tw = fontAtlas().measureText(label) + 28.f;
            if (mx >= tx && mx < tx + tw) {
                if (mx >= tx + tw - 18.f) { closeTab(i); return true; }
                switchToTab(i); return true;
            }
            tx += tw + 1.f;
        }
        newBuffer(); return true;
    }
    return false;
}

// ── static data ──

const char* Application::syntaxLanguages[] = {
    "ActionScript","AppleScript","ASP","Batch","C","C#","C++","CSS","Go","HTML",
    "Java","JavaScript","JSON","Lua","Markdown","Objective-C","PHP","Python","Ruby","Rust",
    "SQL","Swift","TOML","XML","YAML","Plain Text"
};

// ── events ──

void Application::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { running_ = false; return; }
        // find mode input
        if (find_.active && e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod; auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { find_.active = false; continue; }
            if (sym == SDLK_RETURN) {
                if (!find_.matches.empty()) {
                    if (mod & KMOD_SHIFT) find_.currentMatch = (find_.currentMatch + find_.matches.size() - 1) % find_.matches.size();
                    else find_.currentMatch = (find_.currentMatch + 1) % find_.matches.size();
                    selections_[0].anchor = selections_[0].cursor = find_.matches[find_.currentMatch];
                    ensureCursorVisible();
                }
                continue;
            }
            if ((mod & KMOD_CTRL) && (sym == SDLK_f || sym == SDLK_h || sym == SDLK_r)) continue;
            if (sym == SDLK_BACKSPACE && !find_.query.empty()) {
                auto it = find_.query.end(); --it;
                while (it != find_.query.begin() && (*it & 0xC0) == 0x80) --it;
                find_.query.erase(it, find_.query.end()); findAllMatches(); continue;
            }
            if ((mod & KMOD_ALT) && sym == SDLK_r) { find_.regex = !find_.regex; findAllMatches(); continue; }
            if ((mod & KMOD_ALT) && sym == SDLK_c) { find_.caseSensitive = !find_.caseSensitive; findAllMatches(); continue; }
            if ((mod & KMOD_ALT) && sym == SDLK_w) { find_.wholeWord = !find_.wholeWord; findAllMatches(); continue; }
            continue;
        }
        if (find_.active && e.type == SDL_TEXTINPUT) { find_.query += e.text.text; findAllMatches(); continue; }
        // goto mode
        if (goto_.active && e.type == SDL_KEYDOWN) {
            auto sym = e.key.keysym.sym;
            if (sym == SDLK_ESCAPE) { goto_.active = false; continue; }
            if (sym == SDLK_RETURN) {
                if (!goto_.items.empty() && goto_.selected >= 0 && goto_.selected < (int)goto_.items.size()) {
                    openFile(goto_.items[goto_.selected]);
                }
                goto_.active = false; continue;
            }
            if (sym == SDLK_UP) { if (goto_.selected > 0) --goto_.selected; continue; }
            if (sym == SDLK_DOWN) { if (goto_.selected < (int)goto_.items.size() - 1) ++goto_.selected; continue; }
            if (sym == SDLK_BACKSPACE && !goto_.query.empty()) {
                auto it = goto_.query.end(); --it;
                while (it != goto_.query.begin() && (*it & 0xC0) == 0x80) --it;
                goto_.query.erase(it, goto_.query.end()); continue;
            }
            continue;
        }
        if (goto_.active && e.type == SDL_TEXTINPUT) { goto_.query += e.text.text; continue; }
        // menu bar
        int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
        if (titlebar_->handleEvent(e, window_)) continue;
        { int tbww,tbwh; SDL_GL_GetDrawableSize(window_,&tbww,&tbwh); if (handleTabBarEvent(e,(float)tbww,titlebar_->height())) continue; }
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            SDL_GL_GetDrawableSize(window_, &ww, &wh); GLRenderer::resize(ww, wh); titlebar_->layout(ww);
        }
        else if (e.type == SDL_MOUSEMOTION && statusPopup_ != StatusPopup::None) {
            int itemCount = (statusPopup_ == StatusPopup::Indent) ? 12 : syntaxLangCount;
            float popW = 240.f, popH = itemCount * 24.f + 4.f;
            if (e.motion.x >= popupX_ && e.motion.x <= popupX_ + popW && e.motion.y >= popupY_ && e.motion.y <= popupY_ + popH)
                popupSelected_ = (int)((e.motion.y - popupY_ - 2) / 24.f);
            else popupSelected_ = -1;
        }
        else if (e.type == SDL_MOUSEWHEEL) {
            scrollY_ -= e.wheel.y * fontAtlas().lineHeight() * 3;
            float lineStep = fontAtlas().lineHeight();
            float contentH = totalLines() * lineStep + 100;
            SDL_GL_GetDrawableSize(window_, &ww, &wh);
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
            float gutterW = gutter_->width();
            float textX = gutterW + 8.0f;
            float sbH = statusbar_->height();
            float fww = (float)ww, fwh = (float)wh;
            // status bar popup click
            if (statusPopup_ != StatusPopup::None) {
                int itemCount = (statusPopup_ == StatusPopup::Indent) ? 12 : syntaxLangCount;
                float popW = 240.f, popH = itemCount * 24.f + 4.f;
                if (mx >= popupX_ && mx <= popupX_ + popW && my >= popupY_ && my <= popupY_ + popH) {
                    int idx = (int)((my - popupY_ - 2) / 24.f);
                    if (idx >= 0) {
                        if (statusPopup_ == StatusPopup::Indent) {
                            if (idx == 0) { useTabs_ = false; syntax_->setUseTabs(false); }
                            else if (idx >= 1 && idx <= 8) { tabSize_ = idx; syntax_->setTabSize(idx); }
                            else if (idx == 9) guessIndent();
                            else if (idx == 10) convertIndentation(true);
                            else if (idx == 11) convertIndentation(false);
                        } else {
                            if (idx < syntaxLangCount) {
                                syntax_->setLanguageByName(syntaxLanguages[idx]);
                                syntaxLangIndex_ = idx;
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
                float editorRight = fww - minimap_->width();
                std::string indentLabel = useTabs_ ? ("Tab Size: " + std::to_string(tabSize_)) : ("Spaces: " + std::to_string(tabSize_));
                float indentW = fontAtlas().measureText(indentLabel);
                std::string synLabel = syntax_->languageName();
                float synW = fontAtlas().measureText(synLabel);
                float indentX = editorRight - indentW - synW - 32.f - 12.f;
                float synX = editorRight - synW - 12.f;
                if (mx >= indentX && mx <= indentX + indentW + 10.f) {
                    statusPopup_ = StatusPopup::Indent; popupX_ = indentX; popupY_ = fwh - sbH - 12 * 24.f - 4.f; popupSelected_ = 0; continue;
                }
                if (mx >= synX && mx <= synX + synW + 10.f) {
                    statusPopup_ = StatusPopup::Syntax; popupX_ = synX; popupY_ = fwh - sbH - syntaxLangCount * 24.f - 4.f; popupSelected_ = 0; continue;
                }
            }
            // gutter fold click
            if (my > tbH && mx < gutterW) {
                float clickY = my + scrollY_ - textOriginY;
                size_t clickLine = (size_t)(clickY / lineStep);
                if (clickLine < totalLines()) {
                    size_t le = lineEnd(lineStartForLine(clickLine));
                    bool bracketFold = (le < textBuffer.size() && (textBuffer[le] == '{' || textBuffer[le] == '[' || textBuffer[le] == '('));
                    bool indentFold = false;
                    if (!bracketFold && clickLine + 1 < totalLines()) {
                        int curInd = (clickLine < lineIndents_.size()) ? lineIndents_[clickLine] : 0;
                        int nxtInd = (clickLine + 1 < lineIndents_.size()) ? lineIndents_[clickLine+1] : 0;
                        indentFold = (nxtInd > curInd);
                    }
                    if (bracketFold || indentFold) { toggleFold(clickLine); continue; }
                }
            }
            if (my > tbH && mx >= gutterW) {
                auto mod = SDL_GetModState();
                float clickY = my + scrollY_ - textOriginY;
                size_t clickLine = (size_t)(clickY / lineStep);
                if (clickLine >= totalLines()) clickLine = totalLines() - 1;
                size_t ls = lineStartForLine(clickLine), le = lineEnd(ls);
                std::string_view lt(textBuffer.data() + ls, le - ls);
                float cx = mx - textX; size_t col = 0; float advance = 0;
                for (size_t i = 0; i < lt.size(); ) {
                    uint32_t cp = (uint8_t)lt[i]; int b = 1;
                    if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                    int cw = (cp == '\t') ? (int)fontAtlas().measureText(" ") * 2 : fontAtlas().getGlyph(cp).advance;
                    if (advance + cw / 2.f > cx) break;
                    advance += cw; col += b; i += b;
                }
                size_t clickPos = ls + col;
                if (mod & KMOD_CTRL) selections_.emplace_back(clickPos);
                else if (mod & KMOD_SHIFT) selections_[0].cursor = clickPos;
                else { selections_.clear(); selections_.emplace_back(clickPos); }
                desiredCursorX_ = -1.f;
            }
        }
        else if (e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod; auto sym = e.key.keysym.sym;
            auto& sel = selections_[0];
            bool shift = mod & KMOD_SHIFT, ctrl = mod & KMOD_CTRL;
            if (ctrl && sym == SDLK_q) running_ = false;
            else if (ctrl && sym == SDLK_z) { if (shift) doRedo(); else doUndo(); }
            else if (ctrl && sym == SDLK_y) doRedo();
            else if (ctrl && sym == SDLK_f) { find_.active = true; find_.replaceActive = false; find_.query.clear(); find_.matches.clear(); }
            else if (ctrl && sym == SDLK_h) { find_.active = true; find_.replaceActive = true; find_.query.clear(); find_.matches.clear(); }
            else if (ctrl && sym == SDLK_s) { if (shift) saveFileAs(); else saveFile(); }
            else if (ctrl && sym == SDLK_p) { goto_.active = true; goto_.query.clear(); goto_.selected = 0; goto_.items.clear(); }
            else if (ctrl && sym == SDLK_o) {
#ifdef _WIN32
                char buf[MAX_PATH] = {}; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = "All Files\0*.*\0"; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) openFile(buf);
#endif
            }
            else if (ctrl && sym == SDLK_n) newBuffer();
            else if (ctrl && sym == SDLK_w) { if (dirty_) saveFile(); newBuffer(); }
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
                    textBuffer.erase(ds, pos - ds);
                    sel.anchor = sel.cursor = ds; dirty_ = true; desiredCursorX_ = -1.f;
                }
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
            else if (sym == SDLK_RETURN) insertText("\n");
            else if (sym == SDLK_TAB) {
                if (shift) {
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
                size_t cl = lineOfPos(sel.cursor); if (cl > 0) {
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
                }
            }
            else if (sym == SDLK_DOWN) {
                size_t cl = lineOfPos(sel.cursor); if (cl < totalLines() - 1) {
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
                }
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
            ensureCursorVisible();
        }
        else if (e.type == SDL_TEXTINPUT) { insertText(e.text.text); }
    }
}

void Application::update() {}
void Application::updateTitle() {
    std::string t = openFile_; if (dirty_) t += "\xe2\x80\xa2"; t += " - Moreno Text";
    titlebar_->setTitle(t); SDL_SetWindowTitle(window_, t.c_str());
}

void Application::render() {
    GLRenderer::beginFrame();
    updateTitle();
    int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
    float fww = (float)ww, fwh = (float)wh;
    titlebar_->draw(fontAtlas(), 0, 0, 0, 0);
    drawTabBar(fontAtlas(), fww, titlebar_->height());
    float tbH = titlebar_->height() + tabBarH_;
    float sbH = statusbar_->height();
    float lineStep = fontAtlas().lineHeight();
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    size_t lineCount = totalLines();
    size_t currentLine = lineOfPos(selections_[0].cursor);
    size_t currentCol = colOfPos(selections_[0].cursor);
    // gutter
    size_t visibleCount = lineCount;
    gutter_->draw(fontAtlas(), visibleCount, currentLine, textOriginY, lineStep, fwh - sbH, tbH);
    float gutterW = gutter_->width();
    float textX = gutterW + 8.0f;
    float editorRight = fww - minimap_->width();
    // scissor
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)gutterW, (int)sbH, ww - (int)gutterW - (int)minimap_->width(), wh - (int)(tbH + sbH));
    // selection highlights
    for (auto& s : selections_) {
        if (!s.hasSelection()) continue;
        size_t a = s.min(), b = s.max(), la = lineOfPos(a), lb = lineOfPos(b);
        std::vector<float> sv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float bl,float a2) {
            sv.insert(sv.end(),{x0,y0,0,0,r,g,bl,a2, x0,y1,0,0,r,g,bl,a2, x1,y1,0,0,r,g,bl,a2, x0,y0,0,0,r,g,bl,a2, x1,y1,0,0,r,g,bl,a2, x1,y0,0,0,r,g,bl,a2});
        };
        for (size_t ln = la; ln <= lb; ++ln) {
            if (isFolded(ln)) continue;
            size_t ls = lineStartForLine(ln), le = lineEnd(ls);
            size_t ss = (ln == la) ? a : ls, se = (ln == lb) ? b : le;
            float sy = textOriginY + ln * lineStep - scrollY_;
            if (sy + lineStep < tbH || sy > fwh) continue;
            float sx = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, ss - ls));
            float ex = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, se - ls));
            ar(sx, sy, ex, sy + lineStep, 0.2f, 0.4f, 0.7f, 0.4f);
        }
        if (!sv.empty()) { GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, sv.size()*sizeof(float), sv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(sv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0); }
    }
    // find match highlights
    if (find_.active && !find_.matches.empty()) {
        std::vector<float> fv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a2) {
            fv.insert(fv.end(),{x0,y0,0,0,r,g,b,a2, x0,y1,0,0,r,g,b,a2, x1,y1,0,0,r,g,b,a2, x0,y0,0,0,r,g,b,a2, x1,y1,0,0,r,g,b,a2, x1,y0,0,0,r,g,b,a2});
        };
        size_t qLen = find_.query.empty() ? 1 : find_.query.size();
        for (size_t mi = 0; mi < find_.matches.size(); ++mi) {
            size_t m = find_.matches[mi]; size_t ln = lineOfPos(m); size_t ls = lineStartForLine(ln);
            float sy = textOriginY + ln * lineStep - scrollY_;
            if (sy + lineStep < tbH || sy > fwh) continue;
            float sx = textX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, m-ls));
            float ex = textX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, m+qLen-ls));
            bool cur = (mi == find_.currentMatch);
            ar(sx, sy, ex, sy + lineStep, cur?0.8f:0.6f, cur?0.7f:0.6f, 0.2f, 0.5f);
        }
        if (!fv.empty()) { GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, fv.size()*sizeof(float), fv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(fv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0); }
    }
    // text lines with syntax highlighting
    // indent guides + fold ... + whitespace dots
    std::vector<float> guideVerts;
    auto addGuideRect = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        guideVerts.insert(guideVerts.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    float spaceWidth = fontAtlas().measureText(" ");
    float y = textOriginY - scrollY_;
    size_t lineIdx = 0, lStart = 0;
    // build indent guide data: track per-line indent
    computeLineIndents();
    // draw indent guides (vertical 1px lines at each indent level)
    {
        float gy = textOriginY - scrollY_;
        for (size_t ln = 0; ln < totalLines(); ++ln) {
            if (isFolded(ln)) continue;
            if (gy + lineStep < tbH) { gy += lineStep; continue; }
            if (gy > fwh) break;
            int indent = (ln < lineIndents_.size()) ? lineIndents_[ln] : 0;
            for (int lvl = tabSize_; lvl < indent; lvl += tabSize_) {
                float gx = textX + lvl * spaceWidth;
                // only draw if next line has same or deeper indent
                bool draw = (ln + 1 < totalLines() && lineIndents_[ln+1] > lvl);
                // or previous line has deeper indent
                draw = draw || (ln > 0 && lineIndents_[ln-1] >= lvl);
                if (draw) addGuideRect(gx, gy, gx + 1, gy + lineStep, 0.3f, 0.3f, 0.35f, 0.5f);
            }
            gy += lineStep;
        }
    }
    // flush indent guides
    if (!guideVerts.empty()) {
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, guideVerts.size()*sizeof(float), guideVerts.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(guideVerts.size()/8));
        glBindVertexArray(0); GLRenderer::setDrawMode(0);
    }
    // text lines
    y = textOriginY - scrollY_;
    lineIdx = 0; lStart = 0;
    while (lStart <= textBuffer.size()) {
        size_t lEnd = textBuffer.find('\n', lStart);
        if (lEnd == std::string::npos) lEnd = textBuffer.size();
        if (!isFolded(lineIdx) && y + lineStep > tbH && y < fwh - sbH) {
            std::string_view line(textBuffer.data() + lStart, lEnd - lStart);
            auto tokens = syntax_->highlightLine(line, lStart);
            if (tokens.empty()) {
                auto& c = syntax_->scopeColor(0);
                fontAtlas().drawText(line, textX, y, c.r, c.g, c.b, 1.0f);
            } else {
                float cx = textX;
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
        }
        y += isFolded(lineIdx) ? 0 : lineStep;
        ++lineIdx; lStart = lEnd + 1;
        if (y > fwh) break;
    }
    // folded line ... indicators
    {
        float fy = textOriginY - scrollY_;
        for (size_t ln = 0; ln < totalLines(); ++ln) {
            if (isFolded(ln) && fy + lineStep > tbH && fy < fwh) {
                fontAtlas().drawText("\xe2\x80\xa6", textX, fy, 0.5f, 0.5f, 0.3f, 1.f); // \u2026 ellipsis
            }
            fy += lineStep;
        }
    }
    // whitespace dots in selection
    {
        for (auto& s : selections_) {
            if (!s.hasSelection()) continue;
            size_t sa = s.min(), sb = s.max();
            std::vector<float> wv;
            auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
                wv.insert(wv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
            };
            for (size_t pos = sa; pos < sb; ++pos) {
                char c = textBuffer[pos];
                if (c != ' ' && c != '\t') continue;
                size_t ln = lineOfPos(pos), ls = lineStartForLine(ln);
                float wy = textOriginY + ln * lineStep - scrollY_;
                if (wy + lineStep < tbH || wy > fwh) continue;
                float colOff = 0;
                // calculate pixel X for this position
                std::string_view before(textBuffer.data() + ls, pos - ls);
                float wx = textX + fontAtlas().measureText(before);
                if (c == ' ') {
                    float cx = wx + spaceWidth / 2.f, cy = wy + lineStep / 2.f, r = 1.2f;
                    ar(cx-r, cy-r, cx+r, cy+r, 0.45f, 0.45f, 0.5f, 0.6f);
                } else {
                    // tab arrow: small right-pointing triangle
                    float cx = wx + 2.f, cy = wy + lineStep * 0.3f;
                    ar(cx, cy, cx+5, cy+lineStep*0.2f, 0.45f, 0.45f, 0.5f, 0.6f);
                    ar(cx, cy+lineStep*0.4f, cx+5, cy+lineStep*0.2f, 0.45f, 0.45f, 0.5f, 0.6f);
                }
            }
            if (!wv.empty()) { GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, wv.size()*sizeof(float), wv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(wv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0); }
        }
    }
    // fold indicators in gutter (triangles for foldable lines)
    {
        std::vector<float> fv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            fv.insert(fv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        float gy = textOriginY - scrollY_;
        for (size_t ln = 0; ln < totalLines(); ++ln) {
            if (gy + lineStep < tbH || gy > fwh) { gy += lineStep; continue; }
            size_t le = lineEnd(lineStartForLine(ln));
            bool bracketFold = (le < textBuffer.size() && (textBuffer[le] == '{' || textBuffer[le] == '[' || textBuffer[le] == '('));
            bool indentFold = false;
            if (!bracketFold && ln + 1 < totalLines()) {
                int curInd = (ln < lineIndents_.size()) ? lineIndents_[ln] : 0;
                int nxtInd = (ln + 1 < lineIndents_.size()) ? lineIndents_[ln+1] : 0;
                indentFold = (nxtInd > curInd);
            }
            bool foldable = bracketFold || indentFold;
            if (foldable) {
                float tx = 4.f, ty = gy + lineStep / 2.f - 3.f;
                if (isFolded(ln)) { ar(tx, ty, tx+6, ty+6, 0.6f,0.6f,0.3f,1.f); }
                else { ar(tx, ty+6, tx+3, ty, 0.5f,0.5f,0.55f,1.f); ar(tx+3, ty, tx+6, ty+6, 0.5f,0.5f,0.55f,1.f); }
            }
            gy += lineStep;
        }
        if (!fv.empty()) { GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, fv.size()*sizeof(float), fv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(fv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0); }
    }
    // cursors
    for (auto& s : selections_) {
        size_t cl = lineOfPos(s.cursor), ls = lineStartForLine(cl);
        float cx = textX + fontAtlas().measureText(std::string_view(textBuffer.data()+ls, s.cursor-ls));
        float cy = textOriginY + cl * lineStep - scrollY_;
        if (cy + lineStep < tbH || cy > fwh) continue;
        float ct = cy, cb = cy + fontAtlas().ascent() - fontAtlas().descent();
        std::vector<float> cv = {cx,ct,0,0,.5f,.8f,1.f,1.f, cx,cb,0,0,.5f,.8f,1.f,1.f, cx+2,cb,0,0,.5f,.8f,1.f,1.f, cx,ct,0,0,.5f,.8f,1.f,1.f, cx+2,cb,0,0,.5f,.8f,1.f,1.f, cx+2,ct,0,0,.5f,.8f,1.f,1.f};
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, cv.size()*sizeof(float), cv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, fontAtlas().atlasTexture()); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0);
    }
    glDisable(GL_SCISSOR_TEST);
    // minimap
    minimap_->draw(fontAtlas(), textBuffer, editorRight, textOriginY, fwh, tbH, gutterW, lineStep);
    // status bar
    statusbar_->draw(fontAtlas(), fww, fwh, minimap_->width(), currentLine, currentCol, syntax_->languageName(), useTabs_, tabSize_, "");
    // find bar
    if (find_.active) {
        float barH = find_.replaceActive ? 64.f : 32.f, barY = fwh - sbH - barH;
        std::vector<float> fbv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            fbv.insert(fbv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        ar(0, barY, editorRight, barY + barH, 0.18f, 0.18f, 0.21f, 1.f);
        ar(0, barY, editorRight, barY + 1, 0.3f, 0.3f, 0.35f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, fbv.size()*sizeof(float), fbv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(fbv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        std::string fl = "Find: " + find_.query + "|";
        if (find_.regex) fl += "  [REGEX]"; if (find_.caseSensitive) fl += "  [CASE]"; if (find_.wholeWord) fl += "  [WORD]";
        if (!find_.matches.empty()) fl += "  (" + std::to_string(find_.currentMatch+1) + "/" + std::to_string(find_.matches.size()) + ")";
        fontAtlas().drawText(fl, 12.f, barY + 6.f, 0.8f, 0.8f, 0.8f, 1.f);
        if (find_.replaceActive) {
            std::string rl = "Replace: " + find_.replace + "|";
            fontAtlas().drawText(rl, 12.f, barY + 28.f, 0.7f, 0.7f, 0.7f, 1.f);
            // buttons
            float bx = editorRight - 280.f;
            fontAtlas().drawText("[Replace]", bx, barY + 28.f, 0.6f, 0.7f, 0.6f, 1.f);
            fontAtlas().drawText("[Replace All]", bx + 80.f, barY + 28.f, 0.6f, 0.7f, 0.6f, 1.f);
        }
        // toggles
        float tx = editorRight - 240.f;
        fontAtlas().drawText(find_.regex ? "[.*]" : ".*", tx, barY + 6.f, find_.regex ? 0.9f : 0.4f, find_.regex ? 0.8f : 0.4f, 0.3f, 1.f);
        fontAtlas().drawText(find_.caseSensitive ? "[Aa]" : "Aa", tx + 40.f, barY + 6.f, find_.caseSensitive ? 0.9f : 0.4f, find_.caseSensitive ? 0.8f : 0.4f, 0.3f, 1.f);
        fontAtlas().drawText(find_.wholeWord ? "[\\b]" : "\\b", tx + 80.f, barY + 6.f, find_.wholeWord ? 0.9f : 0.4f, find_.wholeWord ? 0.8f : 0.4f, 0.3f, 1.f);
    }
    // goto overlay
    if (goto_.active) {
        float ow = 400.f, oh = 200.f, ox = (fww - ow) / 2.f, oy = tbH + 20.f;
        std::vector<float> gv;
        auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
            gv.insert(gv.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
        };
        ar(ox, oy, ox + ow, oy + oh, 0.15f, 0.15f, 0.18f, 0.97f);
        ar(ox, oy, ox + ow, oy + 28, 0.12f, 0.12f, 0.14f, 1.f);
        // selected item highlight
        if (goto_.selected >= 0 && goto_.selected < (int)goto_.items.size())
            ar(ox, oy + 28 + goto_.selected * 22, ox + ow, oy + 28 + (goto_.selected + 1) * 22, 0.22f, 0.28f, 0.42f, 1.f);
        GLRenderer::setDrawMode(2); glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo()); glBufferData(GL_ARRAY_BUFFER, gv.size()*sizeof(float), gv.data(), GL_DYNAMIC_DRAW); glBindTexture(GL_TEXTURE_2D, 0); glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(gv.size()/8)); glBindVertexArray(0); GLRenderer::setDrawMode(0);
        fontAtlas().drawText("Goto: " + goto_.query + "|", ox + 8, oy + 6, 0.8f, 0.8f, 0.8f, 1.f);
        for (int i = 0; i < (int)goto_.items.size() && i < 7; ++i) {
            float ib = (i == goto_.selected) ? 1.f : 0.7f;
            fontAtlas().drawText(goto_.items[i], ox + 8, oy + 30 + i * 22, ib, ib, ib, 1.f);
        }
    }
    // status bar popup (indent settings or syntax picker)
    if (statusPopup_ != StatusPopup::None) {
        int itemCount = (statusPopup_ == StatusPopup::Indent) ? 12 : syntaxLangCount;
        // compute popup width from content
        float maxTextW = 100.f;
        if (statusPopup_ == StatusPopup::Indent) {
            for (int i = 1; i <= 8; ++i) { char tb[48]; snprintf(tb,sizeof(tb),"  Tab Width: %d",i); float w=fontAtlas().measureText(tb)+32.f; if(w>maxTextW) maxTextW=w; }
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
        glScissor((int)popupX_, (int)(popupY_), (int)popW + 2, (int)popH + 2);
        if (statusPopup_ == StatusPopup::Indent) {
            fontAtlas().drawText(useTabs_ ? "  \xe2\x80\xa2  Indent Using Spaces" : "  \xe2\x80\xa2  Indent Using Spaces", popupX_ + 8, popupY_ + 6, 0.75f, 0.75f, 0.78f, 1.f);
            for (int i = 1; i <= 8; ++i) {
                char tb[32]; snprintf(tb, sizeof(tb), "  %s  Tab Width: %d", (tabSize_ == i && !useTabs_) ? "\xe2\x80\xa2" : " ", i);
                fontAtlas().drawText(tb, popupX_ + 8, popupY_ + 6 + i * 24.f, (tabSize_ == i && !useTabs_) ? 0.9f : 0.7f, (tabSize_ == i && !useTabs_) ? 0.9f : 0.7f, (tabSize_ == i && !useTabs_) ? 1.f : 0.75f, 1.f);
            }
            fontAtlas().drawText("Guess Settings From Buffer", popupX_ + 24, popupY_ + 6 + 9 * 24.f, 0.7f, 0.7f, 0.75f, 1.f);
            fontAtlas().drawText("Convert Indentation to Spaces", popupX_ + 24, popupY_ + 6 + 10 * 24.f, 0.7f, 0.7f, 0.75f, 1.f);
            fontAtlas().drawText("Convert Indentation to Tabs", popupX_ + 24, popupY_ + 6 + 11 * 24.f, 0.7f, 0.7f, 0.75f, 1.f);
        } else {
            for (int i = 0; i < syntaxLangCount; ++i) {
                float b = (syntax_->languageName() == syntaxLanguages[i]) ? 1.f : 0.7f;
                if (syntax_->languageName() == syntaxLanguages[i])
                    fontAtlas().drawText("\xe2\x80\xa2", popupX_ + 8, popupY_ + 6 + i * 24.f, 0.9f, 0.9f, 1.f, 1.f);
                fontAtlas().drawText(syntaxLanguages[i], popupX_ + 24, popupY_ + 6 + i * 24.f, b, b, b, 1.f);
            }
        }
        glDisable(GL_SCISSOR_TEST);
    }
    GLRenderer::endFrame();
    SDL_GL_SwapWindow(window_);
}

void Application::shutdown() {
    delete syntax_; delete statusbar_; delete minimap_; delete gutter_; delete titlebar_;
    fontAtlas().destroy(); GLRenderer::destroy();
    SDL_StopTextInput(); SDL_GL_DeleteContext(glContext_); SDL_DestroyWindow(window_); SDL_Quit();
}

int main(int argc, char** argv) { return Application::instance().run(argc, argv); }
