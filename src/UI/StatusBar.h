#pragma once
#include <string>

class StatusBar {
public:
    void draw(class FontAtlas& font, float windowW, float windowH, float minimapW,
              size_t line, size_t col, const std::string& syntaxName, int spaces, const std::string& branch);
    float height() const { return height_; }
private:
    float height_ = 22.f;
};
