#pragma once
class MenuBar {
public:
    MenuBar() = default;
    void init() {}
    void draw(class FontAtlas&, float, float) {}
    bool handleEvent(const union SDL_Event&, float, float) { return false; }
    float height() const { return 0.f; }
    bool isOpen() const { return false; }
};
