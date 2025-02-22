#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <cstdio>
#include <filesystem>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

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
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
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
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            int w, h;
            SDL_GL_GetDrawableSize(window_, &w, &h);
            GLRenderer::resize(w, h);
        } else if (e.type == SDL_TEXTINPUT) {
            textBuffer += e.text.text;
        } else if (e.type == SDL_KEYDOWN) {
            auto mod = e.key.keysym.mod;
            auto sym = e.key.keysym.sym;
            if ((mod & KMOD_CTRL) && sym == SDLK_q) running_ = false;
            else if (sym == SDLK_BACKSPACE && !textBuffer.empty()) {
                auto it = textBuffer.end(); --it;
                while (it != textBuffer.begin() && (*it & 0xC0) == 0x80) { --it; }
                textBuffer.erase(it, textBuffer.end());
            } else if (sym == SDLK_RETURN) {
                textBuffer += '\n';
            }
        }
    }
}

void Application::update() {}

void Application::render() {
    GLRenderer::beginFrame();
    float y = 10.0f;
    size_t lineStart = 0;
    while (lineStart <= textBuffer.size()) {
        size_t lineEnd = textBuffer.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = textBuffer.size();
        std::string_view line(textBuffer.data() + lineStart, lineEnd - lineStart);
        fontAtlas().drawText(line, 10.0f, y, 0.85f, 0.85f, 0.85f, 1.0f);
        y += fontAtlas().lineHeight();
        lineStart = lineEnd + 1;
        if (y > 720) break;
    }
    fontAtlas().drawText("|", 10.0f, y, 0.5f, 0.8f, 1.0f, 1.0f);
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
