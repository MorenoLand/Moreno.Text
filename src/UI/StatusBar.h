#pragma once
#include <string>

class StatusBar {
public:
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
