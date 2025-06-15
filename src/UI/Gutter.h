#pragma once
#include <string>

class FontAtlas;

class Gutter {
public:
    Gutter() = default;
    void draw(FontAtlas& font, size_t totalLines, size_t currentLine,
              float originY, float lineStep, float windowH, float titlebarH);
    float width() const { return width_; }
private:
    float width_ = 48.f;
    float padding_ = 8.f;
    static int digitCount(size_t n);
};
