#pragma once
#include <GL/glew.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <string>
#include <string_view>
#include <unordered_map>

struct Glyph {
    GLuint texID = 0;
    int width = 0, height = 0;
    int bearingX = 0, bearingY = 0;
    int advance = 0;
};

class FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas() { destroy(); }
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;
    bool init(const std::string& fontPath, int fontSize = 16);
    void destroy();
    const Glyph& getGlyph(uint32_t codepoint);
    void drawText(std::string_view text, float x, float y, float r, float g, float b, float a);
    float lineHeight() const { return lineHeight_; }
private:
    void loadGlyph(uint32_t cp);
    FT_Library ftLib_ = nullptr;
    FT_Face ftFace_ = nullptr;
    std::unordered_map<uint32_t, Glyph> glyphs_;
    float lineHeight_ = 0;
};

FontAtlas& fontAtlas();
