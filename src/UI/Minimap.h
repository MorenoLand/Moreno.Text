#pragma once
#include <string>
#include <vector>
#include <algorithm>

class FontAtlas;
class SyntaxHighlighter;

class Minimap {
public:
    struct LineWindow { size_t first; size_t count; };
    Minimap() = default;
    static LineWindow visibleLineWindow(size_t lineCount, float minimapHeight, float miniLineStep) {
        if (lineCount == 0 || minimapHeight <= 0.f || miniLineStep <= 0.f) return {0, 0};
        size_t visible = static_cast<size_t>(minimapHeight / miniLineStep) + 2;
        return {0, std::min(lineCount, visible)};
    }
    void draw(FontAtlas& font, SyntaxHighlighter& syntax, const std::string& buffer,
              float originX, float originY, float windowH, float titlebarH,
              float gutterW, float lineStep, float scrollY, float maxScrollParam, bool hovered);
    float width() const { return width_; }
    float contentWidth() const { return width_; }
    bool handleClick(float mx, float my, float& scrollRatio);
    bool isDirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markClean() { dirty_ = false; }
    void setMouseOver(bool over) { mouseOver_ = over; }
    bool mouseOver() const { return mouseOver_; }
    void updateHoverFade(float dt);
private:
    float minimapWidth_ = 120.f;
    float width_ = minimapWidth_;
    bool dirty_ = true;
    int lastLineCount_ = -1;
    bool mouseOver_ = false;
    float hoverFade_ = 0.2f;
    size_t cachedBufferSize_ = 0;
    std::vector<size_t> lineOffsets_{0};
    void rebuildLineOffsets(const std::string& buffer);
};
