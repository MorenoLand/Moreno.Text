#pragma once
#include <SDL2/SDL.h>
#include <string>

class Titlebar;
class Gutter;

struct AppPaths {
    std::string exeDir;
    std::string dataDir;
    std::string packagesDir;
    std::string installedPackagesDir;
    std::string cacheDir;
    std::string localDir;
    std::string libDir;
    std::string backupDir;
    bool portable = false;
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
    SDL_Window* window_ = nullptr;
    SDL_GLContext glContext_ = nullptr;
    AppPaths paths_;
    bool running_ = true;
    std::string textBuffer;
    Titlebar* titlebar_ = nullptr;
    Gutter* gutter_ = nullptr;
    friend SDL_HitTestResult hitTestCallback(SDL_Window*, const SDL_Point*, void*);
};
