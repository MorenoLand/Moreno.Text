#pragma once
#include <string>

class FontAtlas;
class SyntaxHighlighter;

class Minimap {
public:
    Minimap() = default;
    void draw(FontAtlas& font, SyntaxHighlighter& syntax, const std::string& buffer,
              float originX, float originY, float windowH, float titlebarH,
              float gutterW, float lineStep, float scrollY, bool hovered);
    float width() const { return width_; }
    float contentWidth() const { return width_; }
    bool handleClick(float mx, float my, float& scrollRatio);
    bool isDirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markClean() { dirty_ = false; }
private:
    float width_ = 100.f;
    bool dirty_ = true;
    int lastLineCount_ = -1;
};
