#pragma once
#include <GL/glew.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct AtlasGlyph {
    float u0, v0, u1, v1;
    int width, height;
    int bearingX, bearingY;
    int advance;
    bool colored = false;
};

class FontAtlas {
public:
    FontAtlas() = default;
    ~FontAtlas() { destroy(); }
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;
    bool init(const std::string& fontPath, int fontSize = 16);
    void destroy();
    const AtlasGlyph& getGlyph(uint32_t codepoint);
    static size_t visibleGlyphReserve(size_t textSize, float clipLeft, float clipRight, float minGlyphAdvance) {
        if (textSize == 0 || clipRight <= clipLeft || minGlyphAdvance <= 0.f) return 0;
        size_t visible = static_cast<size_t>((clipRight - clipLeft) / minGlyphAdvance) + 8;
        return visible < textSize ? visible : textSize;
    }
    void drawText(std::string_view text, float x, float y, float r, float g, float b, float a);
    void drawTextClipped(std::string_view text, float x, float y, float clipLeft, float clipRight, float r, float g, float b, float a);
    void drawTextScaled(std::string_view text, float x, float y, float scale, float r, float g, float b, float a);
    void drawTextScaledClipped(std::string_view text, float x, float y, float scale, float clipLeft, float clipRight, float r, float g, float b, float a);
    float measureText(std::string_view text);
    void resetMeasureCache() const { measureCache_.clear(); }
    float lineHeight() const { return lineHeight_; }
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }
    GLuint atlasTexture() const { return atlasTex_; }
private:
    void loadGlyph(uint32_t cp);
    bool loadFallbackFace(int fontSize);
    bool uploadAtlas();
    FT_Library ftLib_ = nullptr;
    FT_Face ftFace_ = nullptr;
    FT_Face fallbackFace_ = nullptr;
    std::unordered_map<uint32_t, AtlasGlyph> glyphs_;
    float lineHeight_ = 0;
    float ascent_ = 0;
    float descent_ = 0;
    GLuint atlasTex_ = 0;
    int atlasW_ = 1024, atlasH_ = 1024;
    std::vector<uint8_t> atlasData_;
    mutable std::unordered_map<std::string, float> measureCache_;
    bool dirty_ = false;
    int packCursorX_ = 0, packCursorY_ = 0, packRowH_ = 0;
    bool packGlyph(int w, int h, int& outX, int& outY);
};

FontAtlas& fontAtlas();
