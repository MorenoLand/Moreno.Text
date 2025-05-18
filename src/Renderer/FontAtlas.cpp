#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

bool FontAtlas::init(const std::string& fontPath, int fontSize) {
    if (FT_Init_FreeType(&ftLib_)) { fprintf(stderr, "FT_Init_FreeType failed\n"); return false; }
    if (FT_New_Face(ftLib_, fontPath.c_str(), 0, &ftFace_)) { fprintf(stderr, "Failed to load font: %s\n", fontPath.c_str()); return false; }
    FT_Set_Pixel_Sizes(ftFace_, 0, fontSize);
    lineHeight_ = static_cast<float>(ftFace_->size->metrics.height >> 6);
    ascent_ = static_cast<float>(ftFace_->size->metrics.ascender >> 6);
    descent_ = static_cast<float>(ftFace_->size->metrics.descender >> 6);
    atlasData_.resize(atlasW_ * atlasH_, 0);
    for (uint32_t c = 32; c < 127; ++c) loadGlyph(c);
    if (!uploadAtlas()) return false;
    return true;
}

bool FontAtlas::packGlyph(int w, int h, int& outX, int& outY) {
    if (packCursorX_ + w >= atlasW_) {
        packCursorX_ = 0;
        packCursorY_ += packRowH_;
        packRowH_ = 0;
    }
    if (packCursorY_ + h >= atlasH_) return false;
    outX = packCursorX_;
    outY = packCursorY_;
    packCursorX_ += w + 1;
    packRowH_ = std::max(packRowH_, h + 1);
    return true;
}

void FontAtlas::loadGlyph(uint32_t cp) {
    if (glyphs_.count(cp)) return;
    if (FT_Load_Char(ftFace_, cp, FT_LOAD_RENDER)) { fprintf(stderr, "Failed to load glyph U+%04X\n", cp); return; }
    int w = static_cast<int>(ftFace_->glyph->bitmap.width);
    int h = static_cast<int>(ftFace_->glyph->bitmap.rows);
    AtlasGlyph g;
    g.width = w;
    g.height = h;
    g.bearingX = ftFace_->glyph->bitmap_left;
    g.bearingY = ftFace_->glyph->bitmap_top;
    g.advance = static_cast<int>(ftFace_->glyph->advance.x >> 6);
    if (w == 0 || h == 0) {
        g.u0 = g.v0 = g.u1 = g.v1 = 0;
        glyphs_[cp] = g;
        return;
    }
    int px, py;
    if (!packGlyph(w, h, px, py)) { fprintf(stderr, "Atlas full, cannot pack U+%04X\n", cp); return; }
    for (int row = 0; row < h; ++row) {
        memcpy(&atlasData_[(py + row) * atlasW_ + px], ftFace_->glyph->bitmap.buffer + row * w, w);
    }
    float iw = 1.f / atlasW_, ih = 1.f / atlasH_;
    g.u0 = px * iw;
    g.v0 = py * ih;
    g.u1 = (px + w) * iw;
    g.v1 = (py + h) * ih;
    glyphs_[cp] = g;
    dirty_ = true;
}

bool FontAtlas::uploadAtlas() {
    if (!atlasTex_) glGenTextures(1, &atlasTex_);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlasW_, atlasH_, 0, GL_RED, GL_UNSIGNED_BYTE, atlasData_.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    dirty_ = false;
    return true;
}

const AtlasGlyph& FontAtlas::getGlyph(uint32_t cp) {
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end()) return it->second;
    loadGlyph(cp);
    if (dirty_) uploadAtlas();
    return glyphs_[cp];
}

static uint32_t decodeUtf8(std::string_view text, size_t& i) {
    uint32_t cp = static_cast<uint8_t>(text[i]);
    if (cp >= 0xF0 && i + 3 < text.size()) { cp = (cp & 0x07) << 18 | (text[i + 1] & 0x3F) << 12 | (text[i + 2] & 0x3F) << 6 | (text[i + 3] & 0x3F); i += 4; }
    else if (cp >= 0xE0 && i + 2 < text.size()) { cp = (cp & 0x0F) << 12 | (text[i + 1] & 0x3F) << 6 | (text[i + 2] & 0x3F); i += 3; }
    else if (cp >= 0xC0 && i + 1 < text.size()) { cp = (cp & 0x1F) << 6 | (text[i + 1] & 0x3F); i += 2; }
    else { ++i; }
    return cp;
}

void FontAtlas::drawText(std::string_view text, float x, float y, float r, float g, float b, float a) {
    if (text.empty()) return;
    std::vector<float> verts;
    verts.reserve(text.size() * 6 * 8);
    float cursorX = x;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') { cursorX = x; continue; }
        if (cp == '\t') { cursorX += getGlyph(' ').advance * 4; continue; }
        if (cp == '\r') { cursorX = x; continue; }
        const AtlasGlyph& gl = getGlyph(cp);
        if (gl.width > 0 && gl.height > 0) {
            float x0 = cursorX + gl.bearingX;
            float y0 = y + ascent_ - gl.bearingY;
            float x1 = x0 + gl.width;
            float y1 = y0 + gl.height;
            // tri 1: top-left, bottom-left, bottom-right
            verts.insert(verts.end(), { x0, y0, gl.u0, gl.v0, r, g, b, a });
            verts.insert(verts.end(), { x0, y1, gl.u0, gl.v1, r, g, b, a });
            verts.insert(verts.end(), { x1, y1, gl.u1, gl.v1, r, g, b, a });
            // tri 2: top-left, bottom-right, top-right
            verts.insert(verts.end(), { x0, y0, gl.u0, gl.v0, r, g, b, a });
            verts.insert(verts.end(), { x1, y1, gl.u1, gl.v1, r, g, b, a });
            verts.insert(verts.end(), { x1, y0, gl.u1, gl.v0, r, g, b, a });
        }
        cursorX += gl.advance;
    }
    if (verts.empty()) return;
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
    glBindVertexArray(0);
}

void FontAtlas::destroy() {
    if (atlasTex_) { glDeleteTextures(1, &atlasTex_); atlasTex_ = 0; }
    glyphs_.clear();
    atlasData_.clear();
    if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
    if (ftLib_) { FT_Done_FreeType(ftLib_); ftLib_ = nullptr; }
}

float FontAtlas::measureText(std::string_view text) const {
    float cx = 0;
    int spaceAdv = 0;
    auto it = glyphs_.find(' ');
    if (it != glyphs_.end()) spaceAdv = it->second.advance;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n' || cp == '\r') { cx = 0; continue; }
        if (cp == '\t') { cx += spaceAdv * 4; continue; }
        auto git = glyphs_.find(cp);
        if (git != glyphs_.end()) cx += git->second.advance;
    }
    return cx;
}

FontAtlas& fontAtlas() {
    static FontAtlas atlas;
    return atlas;
}
