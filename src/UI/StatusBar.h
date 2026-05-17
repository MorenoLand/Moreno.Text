#pragma once
#include <string>
#include <vector>

class StatusBar {
public:
    void appendSolidRects(class FontAtlas& font, std::vector<float>& verts, float windowW, float windowH,
                          size_t line, size_t col, const std::string& branch);
    void drawText(class FontAtlas& font, float windowW, float windowH,
                  size_t line, size_t col, const std::string& syntaxName, bool useTabs, int tabSize, const std::string& branch);
    void draw(class FontAtlas& font, float windowW, float windowH, float minimapW,
              size_t line, size_t col, const std::string& syntaxName, bool useTabs, int tabSize, const std::string& branch);
    float height() const { return height_; }
    float indentLabelX() const { return indentLabelX_; }
    float indentLabelW() const { return indentLabelW_; }
    float syntaxLabelX() const { return syntaxLabelX_; }
    float syntaxLabelW() const { return syntaxLabelW_; }
private:
    float height_ = 22.f;
    float indentLabelX_ = 0, indentLabelW_ = 0;
    float syntaxLabelX_ = 0, syntaxLabelW_ = 0;
};
