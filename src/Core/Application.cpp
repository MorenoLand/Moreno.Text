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
#include <filesystem>
#include <cstdlib>
#include <vector>
#include <algorithm>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static SDL_HitTestResult hitTestCallback(SDL_Window* win, const SDL_Point* area, void* userdata) {
    auto* app = static_cast<Application*>(userdata);
    auto* tb = app->titlebar_;
    return tb ? tb->hitTest(area->x, area->y, win) : SDL_HITTEST_NORMAL;
}

Application& Application::instance() {
    static Application app;
    return app;
}

int Application::run(int argc, char** argv) {
    if (!init(argc, argv)) return 1;
    while (running_) {
        handleEvents();
        update();
        render();
    }
    shutdown();
    return 0;
}

static std::string findMonospaceFont() {
    static const char* candidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf",
        "C:/Windows/Fonts/lucon.ttf",
#elif __APPLE__
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/Library/Fonts/Menlo.ttc",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/usr/share/fonts/google-noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
#endif
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        if (fs::exists(candidates[i])) return candidates[i];
    }
    return "";
}

bool Application::init(int argc, char** argv) {
    initPaths();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    window_ = SDL_CreateWindow(
        "Moreno Text",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_BORDERLESS
    );
    if (!window_) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, glContext_);
    SDL_GL_SetSwapInterval(1);
    glewExperimental = GL_TRUE;
    if (GLenum err = glewInit(); err != GLEW_OK) {
        fprintf(stderr, "glewInit failed: %s\n", glewGetErrorString(err));
        return false;
    }
    int w, h;
    SDL_GL_GetDrawableSize(window_, &w, &h);
    if (!GLRenderer::init(w, h)) {
        fprintf(stderr, "Renderer init failed\n");
        return false;
    }
    std::string fontPath = findMonospaceFont();
    if (fontPath.empty()) {
        fprintf(stderr, "No monospace font found on system\n");
        return false;
    }
    if (!fontAtlas().init(fontPath)) {
        fprintf(stderr, "Font atlas init failed\n");
        return false;
    }
    titlebar_ = new Titlebar();
    gutter_ = new Gutter();
    minimap_ = new Minimap();
    titlebar_->init(w);
    // set window icon from ICO
    std::string iconPath = (fs::path(paths_.exeDir) / "Assets" / "moreno_text.ico").string();
    if (!fs::exists(iconPath)) iconPath = (fs::path(paths_.exeDir) / ".." / "Assets" / "moreno_text.ico").string();
    if (fs::exists(iconPath)) {
        SDL_RWops* rw = SDL_RWFromFile(iconPath.c_str(), "rb");
        if (rw) {
            SDL_Surface* icon = SDL_LoadBMP_RW(rw, 1);
            if (icon) { SDL_SetWindowIcon(window_, icon); SDL_FreeSurface(icon); }
        }
    }
    SDL_SetWindowHitTest(window_, hitTestCallback, this);
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (SDL_GetWindowWMInfo(window_, &wmInfo)) {
#ifdef _WIN32
        Platform::setRoundedCorners(reinterpret_cast<void*>(wmInfo.info.win.window), 8);
#endif
    }
    SDL_StartTextInput();
    return true;
}

void Application::initPaths() {
#ifdef _WIN32
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    paths_.exeDir = fs::path(exePath).parent_path().string();
#else
    paths_.exeDir = fs::canonical("/proc/self/exe").parent_path().string();
#endif
    fs::path base(paths_.exeDir);
    fs::path dataPath = base / "Data";
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

void Application::handleEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) running_ = false;
        else if (titlebar_->handleEvent(e, window_)) continue;
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int w, h;
            SDL_GL_GetDrawableSize(window_, &w, &h);
            GLRenderer::resize(w, h);
            titlebar_->layout(w);
        } else if (e.type == SDL_TEXTINPUT) {
            textBuffer += e.text.text;
            dirty_ = true;
        } else if (e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod;
            auto sym = e.key.keysym.sym;
            if ((mod & KMOD_CTRL) && sym == SDLK_q) running_ = false;
            else if (sym == SDLK_BACKSPACE && !textBuffer.empty()) {
                auto it = textBuffer.end(); --it;
                while (it != textBuffer.begin() && (*it & 0xC0) == 0x80) { --it; }
                textBuffer.erase(it, textBuffer.end());
                dirty_ = true;
            } else if (sym == SDLK_RETURN) {
                textBuffer += '\n';
                dirty_ = true;
            } else if (sym == SDLK_TAB) {
                if (mod & KMOD_SHIFT) {
                    // unindent: remove up to 4 leading spaces from current line
                    size_t lastNL = textBuffer.rfind('\n');
                    size_t lineStart = (lastNL == std::string::npos) ? 0 : lastNL + 1;
                    int removed = 0;
                    while (removed < 4 && lineStart + removed < textBuffer.size() && textBuffer[lineStart + removed] == ' ')
                        ++removed;
                    if (removed > 0) textBuffer.erase(lineStart, removed);
                } else {
                    textBuffer += "    ";
                }
                dirty_ = true;
            }
        }
    }
}

void Application::update() {}

void Application::updateTitle() {
    std::string title = openFile_;
    if (dirty_) title += "\xe2\x80\xa2";  // bullet dot for unsaved
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
    float textOriginY = tbH + fontAtlas().ascent() + 4.0f;
    float lineStep = fontAtlas().lineHeight();
    // count lines
    size_t lineCount = 1;
    for (char c : textBuffer) if (c == '\n') ++lineCount;
    size_t currentLine = lineCount - 1;
    // draw gutter
    gutter_->draw(fontAtlas(), lineCount, currentLine, textOriginY, lineStep, static_cast<float>(wh), tbH);
    float gutterW = gutter_->width();
    float textX = gutterW + 8.0f;
    // draw text lines
    float y = textOriginY;
    size_t lineIdx = 0;
    size_t lineStart = 0;
    while (lineStart <= textBuffer.size()) {
        size_t lineEnd = textBuffer.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = textBuffer.size();
        std::string_view line(textBuffer.data() + lineStart, lineEnd - lineStart);
        fontAtlas().drawText(line, textX, y, 0.85f, 0.85f, 0.85f, 1.0f);
        y += lineStep;
        ++lineIdx;
        lineStart = lineEnd + 1;
        if (y > wh) break;
    }
    // cursor X
    float cursorX = textX;
    size_t lastLineStart = textBuffer.rfind('\n');
    std::string_view cursorLine = (lastLineStart != std::string::npos)
        ? std::string_view(textBuffer.data() + lastLineStart + 1)
        : std::string_view(textBuffer);
    cursorX += fontAtlas().measureText(cursorLine);
    float cursorY = textOriginY + static_cast<float>((lineCount > 0 ? lineCount - 1 : 0)) * lineStep;
    float curTop = cursorY;
    float curBot = cursorY + fontAtlas().ascent() - fontAtlas().descent();
    std::vector<float> cv = {
        cursorX, curTop, 0, 0, 0.0f, 1.0f, 0.0f, 1.0f,
        cursorX, curBot, 0, 0, 0.0f, 1.0f, 0.0f, 1.0f,
        cursorX + 3, curBot, 0, 0, 0.0f, 1.0f, 0.0f, 1.0f,
        cursorX, curTop, 0, 0, 0.0f, 1.0f, 0.0f, 1.0f,
        cursorX + 3, curBot, 0, 0, 0.0f, 1.0f, 0.0f, 1.0f,
        cursorX + 3, curTop, 0, 0, 0.0f, 1.0f, 0.0f, 1.0f
    };
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, cv.size() * sizeof(float), cv.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, fontAtlas().atlasTexture());
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    // minimap
    float minimapX = static_cast<float>(ww) - minimap_->width();
    minimap_->draw(fontAtlas(), textBuffer, minimapX, textOriginY, static_cast<float>(wh), tbH, gutterW, lineStep);
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
