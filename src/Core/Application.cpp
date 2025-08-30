#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "UI/Titlebar.h"
#include "UI/Gutter.h"
#include "UI/Minimap.h"
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

static SDL_HitTestResult hitTestCallback(SDL_Window* win, const SDL_Point* area, void* userdata) {
    auto* app = static_cast<Application*>(userdata);
    return app->titlebar_ ? app->titlebar_->hitTest(area->x, area->y, win) : SDL_HITTEST_NORMAL;
}

Application& Application::instance() {
    static Application app;
    return app;
}

int Application::run(int argc, char** argv) {
    if (!init(argc, argv)) return 1;
    while (running_) { handleEvents(); update(); render(); }
    shutdown();
    return 0;
}

static std::string findMonospaceFont() {
    static const char* candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf", "C:/Windows/Fonts/lucon.ttf",
#elif __APPLE__
        "/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc", "/Library/Fonts/Menlo.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
#endif
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) if (fs::exists(candidates[i])) return candidates[i];
    return "";
}

bool Application::init(int argc, char** argv) {
    initPaths();
    selections_.emplace_back(0);
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
    if (!glContext_) { fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError()); return false; }
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);
    glewExperimental = GL_TRUE;
    if (GLenum err = glewInit(); err != GLEW_OK) { fprintf(stderr, "glewInit: %s\n", glewGetErrorString(err)); return false; }
    int w, h;
    SDL_GL_GetDrawableSize(window_, &w, &h);
    if (!GLRenderer::init(w, h)) { fprintf(stderr, "Renderer init failed\n"); return false; }
    std::string fontPath = findMonospaceFont();
    if (fontPath.empty() || !fontAtlas().init(fontPath)) { fprintf(stderr, "Font init failed\n"); return false; }
    titlebar_ = new Titlebar();
    gutter_ = new Gutter();
    minimap_ = new Minimap();
    titlebar_->init(w);
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
    // open file from command line arg
    if (argc > 1 && fs::exists(argv[1])) openFile(argv[1]);
    return true;
}

void Application::initPaths() {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH]; GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    paths_.exeDir = fs::path(exePath).parent_path().string();
#else
    paths_.exeDir = fs::canonical("/proc/self/exe").parent_path().string();
#endif
    fs::path base(paths_.exeDir), dataPath = base / "Data";
    if (fs::exists(dataPath)) {
        paths_.portable = true;
        paths_.dataDir = dataPath.string();
        paths_.packagesDir = (dataPath / "Packages").string();
        paths_.installedPackagesDir = (dataPath / "Installed Packages").string();
        paths_.cacheDir = (dataPath / "Cache").string();
        paths_.localDir = (dataPath / "Local").string();
        paths_.libDir = (dataPath / "Lib").string();
        paths_.backupDir = (dataPath / "Backup").string();
    } else {
        std::string configHome;
#ifdef _WIN32
        configHome = std::getenv("APPDATA");
#elif __APPLE__
        configHome = std::string(std::getenv("HOME")) + "/Library/Application Support";
#else
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        configHome = xdg ? xdg : std::string(std::getenv("HOME")) + "/.config";
#endif
        paths_.dataDir = (fs::path(configHome) / "Moreno Text").string();
        paths_.packagesDir = (fs::path(paths_.dataDir) / "Packages").string();
        paths_.installedPackagesDir = (fs::path(paths_.dataDir) / "Installed Packages").string();
        paths_.cacheDir = (fs::path(paths_.dataDir) / "Cache").string();
        paths_.localDir = (fs::path(paths_.dataDir) / "Local").string();
        paths_.libDir = (fs::path(paths_.dataDir) / "Lib").string();
        paths_.backupDir = (fs::path(paths_.dataDir) / "Backup").string();
    }
}

// ── buffer helpers ──

size_t Application::lineStart(size_t pos) const {
    if (pos == 0) return 0;
    size_t p = textBuffer.rfind('\n', pos - 1);
    return p == std::string::npos ? 0 : p + 1;
}

size_t Application::lineEnd(size_t pos) const {
    size_t p = textBuffer.find('\n', pos);
    return p == std::string::npos ? textBuffer.size() : p;
}

size_t Application::lineStartForLine(size_t line) const {
    size_t l = 0, pos = 0;
    while (l < line && pos < textBuffer.size()) {
        size_t nl = textBuffer.find('\n', pos);
        if (nl == std::string::npos) break;
        pos = nl + 1; ++l;
    }
    return pos;
}

size_t Application::lineOfPos(size_t pos) const {
    size_t line = 0;
    for (size_t i = 0; i < pos && i < textBuffer.size(); ++i)
        if (textBuffer[i] == '\n') ++line;
    return line;
}

size_t Application::totalLines() const {
    if (textBuffer.empty()) return 1;
    size_t n = 1;
    for (char c : textBuffer) if (c == '\n') ++n;
    return n;
}

void Application::insertAtCursor(const std::string& text) {
    pushUndo();
    if (selections_[0].hasSelection()) deleteSelection();
    size_t pos = selections_[0].cursor;
    textBuffer.insert(pos, text);
    selections_[0].anchor = selections_[0].cursor = pos + text.size();
    desiredCursorX_ = -1.f;
    dirty_ = true;
}

void Application::deleteSelection() {
    if (!selections_[0].hasSelection()) return;
    size_t a = selections_[0].min(), b = selections_[0].max();
    textBuffer.erase(a, b - a);
    selections_[0].anchor = selections_[0].cursor = a;
    dirty_ = true;
}

void Application::insertText(const std::string& text) {
    pushUndo();
    if (selections_[0].hasSelection()) deleteSelection();
    size_t pos = selections_[0].cursor;
    textBuffer.insert(pos, text);
    selections_[0].anchor = selections_[0].cursor = pos + text.size();
    desiredCursorX_ = -1.f;
    dirty_ = true;
}

void Application::ensureCursorVisible() {
    size_t curLine = lineOfPos(selections_[0].cursor);
    int ww, wh;
    SDL_GL_GetDrawableSize(window_, &ww, &wh);
    float tbH = titlebar_->height();
    float lineStep = fontAtlas().lineHeight();
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    float cursorScreenY = textOriginY + curLine * lineStep - scrollY_;
    if (cursorScreenY < textOriginY) scrollY_ -= (textOriginY - cursorScreenY);
    else if (cursorScreenY + lineStep > wh) scrollY_ += (cursorScreenY + lineStep - wh);
    if (scrollY_ < 0) scrollY_ = 0;
}

void Application::findAllMatches() {
    find_.matches.clear();
    find_.currentMatch = 0;
    if (find_.query.empty()) return;
    if (find_.regex) {
        try {
            std::regex re(find_.query, find_.caseSensitive ? std::regex::ECMAScript : (std::regex::ECMAScript | std::regex::icase));
            auto it = std::sregex_iterator(textBuffer.begin(), textBuffer.end(), re);
            auto end = std::sregex_iterator();
            for (; it != end; ++it) find_.matches.push_back(it->position());
        } catch (...) { return; }
    } else {
        size_t pos = 0;
        std::string hay = find_.caseSensitive ? textBuffer : std::string(textBuffer);
        std::string needle = find_.caseSensitive ? find_.query : std::string(find_.query);
        if (!find_.caseSensitive) {
            auto toLower = [](std::string& s) { for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); };
            toLower(hay); toLower(needle);
        }
        while ((pos = hay.find(needle, pos)) != std::string::npos) {
            find_.matches.push_back(pos);
            pos += needle.size();
        }
    }
}

// ── file ops ──

void Application::openFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::ostringstream ss; ss << f.rdbuf();
    textBuffer = ss.str();
    openFilePath_ = path;
    openFile_ = fs::path(path).filename().string();
    dirty_ = false;
    selections_.clear();
    selections_.emplace_back(textBuffer.size());
    scrollY_ = 0;
    undoStack_.clear();
    redoStack_.clear();
}

void Application::saveFile() {
    if (openFilePath_.empty()) { saveFileAs(); return; }
    std::ofstream f(openFilePath_, std::ios::binary);
    if (!f) return;
    f << textBuffer;
    dirty_ = false;
}

void Application::saveFileAs() {
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = "All Files\0*.*\0Text Files\0*.txt\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "txt";
    if (GetSaveFileNameA(&ofn)) {
        openFilePath_ = buf;
        openFile_ = fs::path(buf).filename().string();
        saveFile();
    }
#endif
}

void Application::newBuffer() {
    textBuffer.clear();
    openFilePath_.clear();
    openFile_ = "untitled";
    dirty_ = false;
    selections_.clear();
    selections_.emplace_back(0);
    scrollY_ = 0;
    undoStack_.clear();
    redoStack_.clear();
}

// ── undo/redo ──

void Application::pushUndo() {
    auto now = std::chrono::steady_clock::now();
    if (!undoStack_.empty() && (now - undoStack_.back().time) < undoWindow_) {
        undoStack_.back().text = textBuffer;
        undoStack_.back().cursorPos = selections_[0].cursor;
        return;
    }
    undoStack_.push_back({textBuffer, selections_[0].cursor, now});
    if (undoStack_.size() > 10000) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}

void Application::doUndo() {
    if (undoStack_.empty()) return;
    redoStack_.push_back({textBuffer, selections_[0].cursor, std::chrono::steady_clock::now()});
    auto& step = undoStack_.back();
    textBuffer = step.text;
    selections_.clear();
    selections_.emplace_back(step.cursorPos);
    undoStack_.pop_back();
    dirty_ = true;
}

void Application::doRedo() {
    if (redoStack_.empty()) return;
    undoStack_.push_back({textBuffer, selections_[0].cursor, std::chrono::steady_clock::now()});
    auto& step = redoStack_.back();
    textBuffer = step.text;
    selections_.clear();
    selections_.emplace_back(step.cursorPos);
    redoStack_.pop_back();
    dirty_ = true;
}

// ── events ──

void Application::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) { running_ = false; return; }
        if (find_.active && e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod;
            auto sym = e.key.keysym.sym;
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
            if ((mod & KMOD_CTRL) && (sym == SDLK_f || sym == SDLK_r)) continue;
            // let text input fall through to update find query
        }
        if (titlebar_->handleEvent(e, window_)) continue;
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int w, h; SDL_GL_GetDrawableSize(window_, &w, &h);
            GLRenderer::resize(w, h); titlebar_->layout(w);
        }
        else if (e.type == SDL_MOUSEWHEEL) {
            scrollY_ -= e.wheel.y * fontAtlas().lineHeight() * 3;
            if (scrollY_ < 0) scrollY_ = 0;
            float lineStep = fontAtlas().lineHeight();
            float contentH = totalLines() * lineStep + 100;
            int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
            float maxScroll = contentH - (wh - titlebar_->height());
            if (maxScroll < 0) maxScroll = 0;
            if (scrollY_ > maxScroll) scrollY_ = maxScroll;
        }
        else if (e.type == SDL_MOUSEBUTTONDOWN) {
            int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
            float mx = static_cast<float>(e.button.x), my = static_cast<float>(e.button.y);
            float tbH = titlebar_->height();
            float gutterW = gutter_->width();
            float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
            float lineStep = fontAtlas().lineHeight();
            float textX = gutterW + 8.0f;
            if (my > tbH && e.button.button == 1) {
                auto mod = SDL_GetModState();
                // click in text area
                float clickY = my + scrollY_ - textOriginY;
                size_t clickLine = static_cast<size_t>(clickY / lineStep);
                if (clickLine >= totalLines()) clickLine = totalLines() - 1;
                size_t ls = lineStartForLine(clickLine);
                size_t le = lineEnd(ls);
                std::string_view lineText(textBuffer.data() + ls, le - ls);
                float cx = mx - textX;
                size_t col = 0;
                float advance = 0;
                for (size_t i = 0; i < lineText.size(); ) {
                    uint32_t cp = static_cast<uint8_t>(lineText[i]);
                    int bytes = 1;
                    if (cp >= 0xF0) bytes = 4; else if (cp >= 0xE0) bytes = 3; else if (cp >= 0xC0) bytes = 2;
                    int charW = 0;
                    if (cp == '\t') charW = fontAtlas().measureText(" ") * 4;
                    else { auto it = fontAtlas().getGlyph(cp); charW = it.advance; }
                    if (advance + charW / 2.f > cx) break;
                    advance += charW;
                    col += bytes;
                    i += bytes;
                }
                size_t clickPos = ls + col;
                if (mod & KMOD_CTRL) {
                    selections_.emplace_back(clickPos);
                } else if (mod & KMOD_SHIFT) {
                    selections_[0].cursor = clickPos;
                } else {
                    selections_.clear();
                    selections_.emplace_back(clickPos);
                }
                desiredCursorX_ = -1.f;
            }
        }
        else if (e.type == SDL_TEXTINPUT) {
            if (find_.active) {
                find_.query += e.text.text;
                findAllMatches();
                continue;
            }
            insertText(e.text.text);
        }
        else if (e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod;
            auto sym = e.key.keysym.sym;
            if (find_.active) {
                if (sym == SDLK_BACKSPACE && !find_.query.empty()) {
                    auto it = find_.query.end(); --it;
                    while (it != find_.query.begin() && (*it & 0xC0) == 0x80) --it;
                    find_.query.erase(it, find_.query.end());
                    findAllMatches();
                }
                continue;
            }
            auto& sel = selections_[0];
            bool shift = mod & KMOD_SHIFT;
            bool ctrl = mod & KMOD_CTRL;
            if (ctrl && sym == SDLK_q) running_ = false;
            else if (ctrl && sym == SDLK_z) { if (shift) doRedo(); else doUndo(); }
            else if (ctrl && sym == SDLK_y) doRedo();
            else if (ctrl && sym == SDLK_f) { find_.active = true; find_.query.clear(); find_.matches.clear(); }
            else if (ctrl && sym == SDLK_s) saveFile();
            else if (ctrl && sym == SDLK_o) {
#ifdef _WIN32
                char buf[MAX_PATH] = {};
                OPENFILENAMEA ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = "All Files\0*.*\0";
                ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) openFile(buf);
#endif
            }
            else if (ctrl && sym == SDLK_n) newBuffer();
            else if (ctrl && sym == SDLK_w) { if (dirty_) saveFile(); newBuffer(); }
            else if (ctrl && sym == SDLK_d) {
                // select next occurrence of current word
                size_t pos = sel.cursor;
                size_t ls = lineStart(pos), le = lineEnd(pos);
                std::string_view lineText(textBuffer.data() + ls, le - ls);
                size_t col = pos - ls;
                size_t wordStart = col, wordEnd = col;
                while (wordStart > 0 && (isalnum(lineText[wordStart-1]) || lineText[wordStart-1] == '_')) --wordStart;
                while (wordEnd < lineText.size() && (isalnum(lineText[wordEnd]) || lineText[wordEnd] == '_')) ++wordEnd;
                if (wordEnd > wordStart) {
                    std::string word(lineText.substr(wordStart, wordEnd - wordStart));
                    size_t searchFrom = sel.hasSelection() ? sel.max() : (ls + wordEnd);
                    size_t found = textBuffer.find(word, searchFrom);
                    if (found == std::string::npos) found = textBuffer.find(word);
                    if (found != std::string::npos) {
                        selections_.emplace_back(found, found + word.size());
                    }
                }
            }
            else if (ctrl && sym == SDLK_l) {
                size_t ls = lineStart(sel.cursor), le = lineEnd(sel.cursor);
                sel.anchor = ls;
                sel.cursor = (le < textBuffer.size()) ? le + 1 : le;
            }
            else if (ctrl && shift && sym == SDLK_l) {
                // split selection into per-line cursors
                if (sel.hasSelection()) {
                    size_t a = sel.min(), b = sel.max();
                    std::vector<SelRange> newSels;
                    size_t pos = a;
                    while (pos <= b) {
                        size_t le = lineEnd(pos);
                        if (le > b) le = b;
                        newSels.emplace_back(pos, le);
                        pos = le + 1;
                        if (pos > b || pos == 0) break;
                    }
                    selections_ = std::move(newSels);
                }
            }
            else if (sym == SDLK_BACKSPACE) {
                pushUndo();
                if (sel.hasSelection()) deleteSelection();
                else if (sel.cursor > 0) {
                    size_t pos = sel.cursor;
                    auto it = textBuffer.begin() + pos; --it;
                    while (it != textBuffer.begin() && (*it & 0xC0) == 0x80) --it;
                    size_t delStart = it - textBuffer.begin();
                    textBuffer.erase(delStart, pos - delStart);
                    sel.anchor = sel.cursor = delStart;
                    dirty_ = true;
                    desiredCursorX_ = -1.f;
                }
            }
            else if (sym == SDLK_DELETE) {
                pushUndo();
                if (sel.hasSelection()) deleteSelection();
                else if (sel.cursor < textBuffer.size()) {
                    size_t pos = sel.cursor;
                    auto it = textBuffer.begin() + pos;
                    size_t delEnd = pos + 1;
                    while (delEnd < textBuffer.size() && (textBuffer[delEnd] & 0xC0) == 0x80) ++delEnd;
                    textBuffer.erase(pos, delEnd - pos);
                    dirty_ = true;
                }
            }
            else if (sym == SDLK_RETURN) { insertText("\n"); }
            else if (sym == SDLK_TAB) {
                if (shift) {
                    pushUndo();
                    size_t ls = lineStart(sel.cursor);
                    int removed = 0;
                    while (removed < 4 && ls + removed < textBuffer.size() && textBuffer[ls + removed] == ' ') ++removed;
                    if (removed > 0) {
                        textBuffer.erase(ls, removed);
                        sel.anchor = sel.cursor = (sel.cursor > ls + removed) ? sel.cursor - removed : ls;
                        dirty_ = true;
                    }
                } else insertText("    ");
            }
            else if (sym == SDLK_LEFT) {
                if (ctrl) {
                    // word left
                    size_t pos = sel.cursor;
                    if (pos > 0) { --pos; while (pos > 0 && (textBuffer[pos] == ' ' || textBuffer[pos] == '\t')) --pos;
                        while (pos > 0 && (isalnum(textBuffer[pos-1]) || textBuffer[pos-1] == '_')) --pos; }
                    if (shift) sel.cursor = pos; else { sel.anchor = sel.cursor = pos; }
                } else {
                    if (sel.cursor > 0) { if (shift) sel.cursor--; else { sel.anchor = sel.cursor = sel.cursor - 1; } }
                    desiredCursorX_ = -1.f;
                }
            }
            else if (sym == SDLK_RIGHT) {
                if (ctrl) {
                    size_t pos = sel.cursor;
                    while (pos < textBuffer.size() && (textBuffer[pos] == ' ' || textBuffer[pos] == '\t')) ++pos;
                    while (pos < textBuffer.size() && (isalnum(textBuffer[pos]) || textBuffer[pos] == '_')) ++pos;
                    if (shift) sel.cursor = pos; else { sel.anchor = sel.cursor = pos; }
                } else {
                    if (sel.cursor < textBuffer.size()) { if (shift) sel.cursor++; else { sel.anchor = sel.cursor = sel.cursor + 1; } }
                    desiredCursorX_ = -1.f;
                }
            }
            else if (sym == SDLK_UP) {
                size_t curLine = lineOfPos(sel.cursor);
                if (curLine > 0) {
                    size_t ls = lineStartForLine(curLine - 1);
                    size_t le = lineEnd(ls);
                    std::string_view lt(textBuffer.data() + ls, le - ls);
                    if (desiredCursorX_ < 0) desiredCursorX_ = fontAtlas().measureText(textBuffer.substr(lineStart(sel.cursor), sel.cursor - lineStart(sel.cursor)));
                    size_t col = 0; float adv = 0;
                    for (size_t i = 0; i < lt.size(); ) {
                        uint32_t cp = static_cast<uint8_t>(lt[i]); int b = 1;
                        if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                        int cw = (cp == '\t') ? fontAtlas().measureText(" ") * 4 : fontAtlas().getGlyph(cp).advance;
                        if (adv + cw / 2.f > desiredCursorX_) break;
                        adv += cw; col += b; i += b;
                    }
                    size_t newPos = ls + col;
                    if (shift) sel.cursor = newPos; else { sel.anchor = sel.cursor = newPos; }
                }
            }
            else if (sym == SDLK_DOWN) {
                size_t curLine = lineOfPos(sel.cursor);
                if (curLine < totalLines() - 1) {
                    size_t nextLS = lineStartForLine(curLine + 1);
                    size_t le = lineEnd(nextLS);
                    std::string_view lt(textBuffer.data() + nextLS, le - nextLS);
                    if (desiredCursorX_ < 0) desiredCursorX_ = fontAtlas().measureText(textBuffer.substr(lineStart(sel.cursor), sel.cursor - lineStart(sel.cursor)));
                    size_t col = 0; float adv = 0;
                    for (size_t i = 0; i < lt.size(); ) {
                        uint32_t cp = static_cast<uint8_t>(lt[i]); int b = 1;
                        if (cp >= 0xF0) b = 4; else if (cp >= 0xE0) b = 3; else if (cp >= 0xC0) b = 2;
                        int cw = (cp == '\t') ? fontAtlas().measureText(" ") * 4 : fontAtlas().getGlyph(cp).advance;
                        if (adv + cw / 2.f > desiredCursorX_) break;
                        adv += cw; col += b; i += b;
                    }
                    size_t newPos = nextLS + col;
                    if (shift) sel.cursor = newPos; else { sel.anchor = sel.cursor = newPos; }
                }
            }
            else if (sym == SDLK_HOME) {
                if (ctrl) { if (shift) sel.cursor = 0; else { sel.anchor = sel.cursor = 0; } }
                else {
                    size_t ls = lineStart(sel.cursor);
                    if (shift) sel.cursor = ls; else { sel.anchor = sel.cursor = ls; }
                }
                desiredCursorX_ = -1.f;
            }
            else if (sym == SDLK_END) {
                if (ctrl) { if (shift) sel.cursor = textBuffer.size(); else { sel.anchor = sel.cursor = textBuffer.size(); } }
                else {
                    size_t le = lineEnd(sel.cursor);
                    if (shift) sel.cursor = le; else { sel.anchor = sel.cursor = le; }
                }
                desiredCursorX_ = -1.f;
            }
            else if (sym == SDLK_PAGEUP) {
                int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
                float visibleLines = (wh - titlebar_->height()) / fontAtlas().lineHeight();
                size_t curLine = lineOfPos(sel.cursor);
                size_t targetLine = (curLine > visibleLines) ? curLine - static_cast<size_t>(visibleLines) : 0;
                size_t pos = lineStartForLine(targetLine);
                if (shift) sel.cursor = pos; else { sel.anchor = sel.cursor = pos; }
                desiredCursorX_ = -1.f;
            }
            else if (sym == SDLK_PAGEDOWN) {
                int ww, wh; SDL_GL_GetDrawableSize(window_, &ww, &wh);
                float visibleLines = (wh - titlebar_->height()) / fontAtlas().lineHeight();
                size_t curLine = lineOfPos(sel.cursor);
                size_t targetLine = curLine + static_cast<size_t>(visibleLines);
                size_t total = totalLines();
                if (targetLine >= total) targetLine = total - 1;
                size_t pos = lineStartForLine(targetLine);
                if (shift) sel.cursor = pos; else { sel.anchor = sel.cursor = pos; }
                desiredCursorX_ = -1.f;
            }
            ensureCursorVisible();
        }
    }
}

void Application::update() {}

void Application::updateTitle() {
    std::string title = openFile_;
    if (dirty_) title += "\xe2\x80\xa2";
    title += " - Moreno Text";
    titlebar_->setTitle(title);
    SDL_SetWindowTitle(window_, title.c_str());
}

void Application::render() {
    GLRenderer::beginFrame();
    updateTitle();
    titlebar_->draw(fontAtlas(), 0, 0, 0, 0);
    float tbH = titlebar_->height();
    int ww, wh;
    SDL_GL_GetDrawableSize(window_, &ww, &wh);
    float lineStep = fontAtlas().lineHeight();
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    size_t lineCount = totalLines();
    size_t currentLine = lineOfPos(selections_[0].cursor);
    // draw gutter
    gutter_->draw(fontAtlas(), lineCount, currentLine, textOriginY, lineStep, static_cast<float>(wh), tbH);
    float gutterW = gutter_->width();
    float textX = gutterW + 8.0f;
    // enable scissor to clip editor area
    glEnable(GL_SCISSOR_TEST);
    glScissor(static_cast<int>(gutterW), 0, ww - static_cast<int>(gutterW) - static_cast<int>(minimap_->width()), wh);
    // draw selection highlights
    for (auto& sel : selections_) {
        if (!sel.hasSelection()) continue;
        size_t a = sel.min(), b = sel.max();
        size_t lineA = lineOfPos(a), lineB = lineOfPos(b);
        std::vector<float> sv;
        auto addRect = [&](float x0, float y0, float x1, float y1, float r, float g, float b2, float a2) {
            sv.insert(sv.end(), { x0,y0, 0,0, r,g,b2,a2 });
            sv.insert(sv.end(), { x0,y1, 0,0, r,g,b2,a2 });
            sv.insert(sv.end(), { x1,y1, 0,0, r,g,b2,a2 });
            sv.insert(sv.end(), { x0,y0, 0,0, r,g,b2,a2 });
            sv.insert(sv.end(), { x1,y1, 0,0, r,g,b2,a2 });
            sv.insert(sv.end(), { x1,y0, 0,0, r,g,b2,a2 });
        };
        for (size_t ln = lineA; ln <= lineB; ++ln) {
            size_t ls = lineStartForLine(ln);
            size_t le = lineEnd(ls);
            size_t selStart = (ln == lineA) ? a : ls;
            size_t selEnd = (ln == lineB) ? b : le;
            float sy = textOriginY + ln * lineStep - scrollY_;
            if (sy + lineStep < tbH || sy > wh) continue;
            float sx = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, selStart - ls));
            float ex = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, selEnd - ls));
            addRect(sx, sy, ex, sy + lineStep, 0.2f, 0.4f, 0.7f, 0.4f);
        }
        if (!sv.empty()) {
            GLRenderer::setDrawMode(2);
            glBindVertexArray(gl_vao());
            glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
            glBufferData(GL_ARRAY_BUFFER, sv.size() * sizeof(float), sv.data(), GL_DYNAMIC_DRAW);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(sv.size() / 8));
            glBindVertexArray(0);
            GLRenderer::setDrawMode(0);
        }
    }
    // draw find matches
    if (find_.active && !find_.matches.empty()) {
        std::vector<float> fv;
        auto addRect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
            fv.insert(fv.end(), { x0,y0, 0,0, r,g,b,a });
            fv.insert(fv.end(), { x0,y1, 0,0, r,g,b,a });
            fv.insert(fv.end(), { x1,y1, 0,0, r,g,b,a });
            fv.insert(fv.end(), { x0,y0, 0,0, r,g,b,a });
            fv.insert(fv.end(), { x1,y1, 0,0, r,g,b,a });
            fv.insert(fv.end(), { x1,y0, 0,0, r,g,b,a });
        };
        size_t qLen = find_.query.empty() ? 1 : find_.query.size();
        for (size_t mi = 0; mi < find_.matches.size(); ++mi) {
            size_t m = find_.matches[mi];
            size_t ln = lineOfPos(m);
            size_t ls = lineStartForLine(ln);
            float sy = textOriginY + ln * lineStep - scrollY_;
            if (sy + lineStep < tbH || sy > wh) continue;
            float sx = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, m - ls));
            float ex = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, m + qLen - ls));
            bool isCurrent = (mi == find_.currentMatch);
            addRect(sx, sy, ex, sy + lineStep, isCurrent ? 0.8f : 0.6f, isCurrent ? 0.7f : 0.6f, 0.2f, 0.5f);
        }
        if (!fv.empty()) {
            GLRenderer::setDrawMode(2);
            glBindVertexArray(gl_vao());
            glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
            glBufferData(GL_ARRAY_BUFFER, fv.size() * sizeof(float), fv.data(), GL_DYNAMIC_DRAW);
            glBindTexture(GL_TEXTURE_2D, 0);
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(fv.size() / 8));
            glBindVertexArray(0);
            GLRenderer::setDrawMode(0);
        }
    }
    // draw text lines (scrolled)
    float y = textOriginY - scrollY_;
    size_t lineStart = 0;
    size_t lineIdx = 0;
    while (lineStart <= textBuffer.size()) {
        size_t lineEnd = textBuffer.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = textBuffer.size();
        if (y + lineStep > tbH && y < wh) {
            std::string_view line(textBuffer.data() + lineStart, lineEnd - lineStart);
            fontAtlas().drawText(line, textX, y, 0.85f, 0.85f, 0.85f, 1.0f);
        }
        y += lineStep;
        ++lineIdx;
        lineStart = lineEnd + 1;
        if (y > wh) break;
    }
    // draw cursors
    for (auto& sel : selections_) {
        size_t curLine = lineOfPos(sel.cursor);
        size_t ls = lineStartForLine(curLine);
        float cursorX = textX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, sel.cursor - ls));
        float cursorY = textOriginY + curLine * lineStep - scrollY_;
        if (cursorY + lineStep < tbH || cursorY > wh) continue;
        float curTop = cursorY;
        float curBot = cursorY + fontAtlas().ascent() - fontAtlas().descent();
        std::vector<float> cv = {
            cursorX, curTop, 0, 0, 0.5f, 0.8f, 1.0f, 1.0f,
            cursorX, curBot, 0, 0, 0.5f, 0.8f, 1.0f, 1.0f,
            cursorX + 2, curBot, 0, 0, 0.5f, 0.8f, 1.0f, 1.0f,
            cursorX, curTop, 0, 0, 0.5f, 0.8f, 1.0f, 1.0f,
            cursorX + 2, curBot, 0, 0, 0.5f, 0.8f, 1.0f, 1.0f,
            cursorX + 2, curTop, 0, 0, 0.5f, 0.8f, 1.0f, 1.0f
        };
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, cv.size() * sizeof(float), cv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, fontAtlas().atlasTexture());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
    }
    glDisable(GL_SCISSOR_TEST);
    // minimap
    float minimapX = static_cast<float>(ww) - minimap_->width();
    minimap_->draw(fontAtlas(), textBuffer, minimapX, textOriginY, static_cast<float>(wh), tbH, gutterW, lineStep);
    // find bar
    if (find_.active) {
        float barH = 32.f, barY = static_cast<float>(wh) - barH;
        std::vector<float> fbv;
        auto addRect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
            fbv.insert(fbv.end(), { x0,y0, 0,0, r,g,b,a }); fbv.insert(fbv.end(), { x0,y1, 0,0, r,g,b,a });
            fbv.insert(fbv.end(), { x1,y1, 0,0, r,g,b,a }); fbv.insert(fbv.end(), { x0,y0, 0,0, r,g,b,a });
            fbv.insert(fbv.end(), { x1,y1, 0,0, r,g,b,a }); fbv.insert(fbv.end(), { x1,y0, 0,0, r,g,b,a });
        };
        addRect(0, barY, static_cast<float>(ww) - minimap_->width(), barY + barH, 0.18f, 0.18f, 0.21f, 1.f);
        addRect(0, barY, static_cast<float>(ww) - minimap_->width(), barY + 1, 0.3f, 0.3f, 0.35f, 1.f);
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, fbv.size() * sizeof(float), fbv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(fbv.size() / 8));
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
        // find label + query
        std::string findLabel = "Find: " + find_.query + "|";
        if (find_.regex) findLabel += "  [REGEX]";
        if (!find_.matches.empty()) findLabel += "  (" + std::to_string(find_.currentMatch + 1) + "/" + std::to_string(find_.matches.size()) + ")";
        fontAtlas().drawText(findLabel, 12.f, barY + 8.f, 0.8f, 0.8f, 0.8f, 1.f);
    }
    GLRenderer::endFrame();
    SDL_GL_SwapWindow(window_);
}

void Application::shutdown() {
    fontAtlas().destroy();
    GLRenderer::destroy();
    SDL_StopTextInput();
    SDL_GL_DeleteContext(glContext_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

int main(int argc, char** argv) {
    return Application::instance().run(argc, argv);
}
