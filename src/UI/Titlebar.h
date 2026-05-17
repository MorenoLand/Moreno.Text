#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <functional>

struct TitlebarMenuItem {
    std::string label;
    std::string shortcut;
    std::function<void()> action;
    bool separator = false;
    int submenu = -1;
};

struct TitlebarButton {
    float x, y, w, h;
    enum Type { CLOSE, MAXIMIZE, MINIMIZE, MENU } type;
    bool hovered = false;
};

class Titlebar {
public:
    Titlebar() = default;
    void init(int windowWidth);
    void layout(int windowWidth);
    void appendSolidRects(std::vector<float>& verts);
    void drawForeground(class FontAtlas& font);
    void draw(class FontAtlas& font, float r, float g, float b, float a);
    bool handleEvent(const SDL_Event& e, SDL_Window* window);
    float height() const { return height_; }
    bool isCustom() const { return custom_; }
    void setCustom(bool c) { custom_ = c; }
    void setTitle(const std::string& t) { title_ = t; }
    SDL_HitTestResult hitTest(int mx, int my, SDL_Window* window);
    bool isMenuOpen() const { return menuOpen_; }
    void closeMenuPopup() { closeMenu(); }
    void deferMenuDraw() { menuDeferred_ = true; }
    void getMenuBounds(float& x, float& y, float& w, float& h) const;
    void getMenuPanelRects(class FontAtlas& font, int windowW, int windowH, SDL_Rect& mainRect, SDL_Rect& submenuRect, bool& hasSubmenu) const;
    void getMenuPopupBounds(class FontAtlas& font, float& x, float& y, float& w, float& h) const;
    void drawMenuPopup(FontAtlas& font, float ox, float oy);
private:
    float height_ = 30.f;
    float buttonSize_ = 46.f;
    float resizeEdge_ = 6.f;
    TitlebarButton buttons_[4];
    bool dragging_ = false;
    bool custom_ = true;
    int dragStartX_ = 0, dragStartY_ = 0;
    int winStartX_ = 0, winStartY_ = 0;
    std::string title_ = "Moreno Text";
    unsigned int iconTex_ = 0;
    int iconW_ = 0, iconH_ = 0;
    void updateButtonPositions(int windowWidth);
    bool menuOpen_ = false;
    int menuHovered_ = -1;
    int submenuOpen_ = -1;
    int submenuHovered_ = -1;
    int menuScroll_ = 0;
    bool menuDeferred_ = false;
    float lastMenuX_ = 0.f, lastMenuY_ = 0.f, lastMenuW_ = 150.f, lastMenuH_ = 0.f;
    float lastSubmenuX_ = 0.f, lastSubmenuY_ = 0.f, lastSubmenuW_ = 0.f, lastSubmenuH_ = 0.f;
    bool lastHasSubmenu_ = false;
    std::vector<TitlebarMenuItem> menuItems_;
    std::vector<std::vector<TitlebarMenuItem>> submenus_;
    void buildMenu();
    void closeMenu() { menuOpen_ = false; menuHovered_ = -1; submenuOpen_ = -1; submenuHovered_ = -1; }
    void drawMenu(class FontAtlas& font);
    bool handleMenuEvent(const SDL_Event& e);
};
