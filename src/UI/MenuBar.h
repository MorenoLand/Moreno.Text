#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <functional>

struct MenuItem {
    std::string label;
    std::string shortcut;
    std::function<void()> action;
    bool separator = false;
    bool enabled = true;
};

struct Menu {
    std::string title;
    std::vector<MenuItem> items;
    float x = 0, w = 0;
    bool open = false;
};

class MenuBar {
public:
    MenuBar() = default;
    void init();
    void draw(class FontAtlas& font, float windowW, float titlebarH);
    bool handleEvent(const SDL_Event& e, float windowW, float titlebarH);
    float height() const { return height_; }
    bool isOpen() const { return anyOpen_; }
private:
    float height_ = 24.f;
    std::vector<Menu> menus_;
    bool anyOpen_ = false;
    int hoveredMenu_ = -1, hoveredItem_ = -1;
    void closeAll();
};
