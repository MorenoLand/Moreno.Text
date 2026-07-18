#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include FT_SYNTHESIS_H

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

static void appendGlyphQuad(std::vector<float>& verts,
                            const AtlasGlyph& gl,
                            float x0, float y0, float x1, float y1,
                            float r, float g, float b, float a) {
    float cr = gl.colored ? -1.f : r;
    float cg = gl.colored ? 0.f : g;
    float cb = gl.colored ? 0.f : b;
    verts.insert(verts.end(), { x0, y0, gl.u0, gl.v0, cr, cg, cb, a,
                                x0, y1, gl.u0, gl.v1, cr, cg, cb, a,
                                x1, y1, gl.u1, gl.v1, cr, cg, cb, a });
    verts.insert(verts.end(), { x0, y0, gl.u0, gl.v0, cr, cg, cb, a,
                                x1, y1, gl.u1, gl.v1, cr, cg, cb, a,
                                x1, y0, gl.u1, gl.v0, cr, cg, cb, a });
}

bool FontAtlas::init(const std::string& fontPath, int fontSize, float backingScale, bool bold) {
    backingScale_ = std::max(1.f, backingScale);
    bold_ = bold;
    int rasterSize = std::max(1, static_cast<int>(std::lround(fontSize * backingScale_)));
    if (FT_Init_FreeType(&ftLib_)) { fprintf(stderr, "FT_Init_FreeType failed\n"); return false; }
    if (FT_New_Face(ftLib_, fontPath.c_str(), 0, &ftFace_)) { fprintf(stderr, "Failed to load font: %s\n", fontPath.c_str()); return false; }
    FT_Set_Pixel_Sizes(ftFace_, 0, rasterSize);
    loadFallbackFace(rasterSize);
    lineHeight_ = static_cast<float>(ftFace_->size->metrics.height >> 6) / backingScale_;
    ascent_ = static_cast<float>(ftFace_->size->metrics.ascender >> 6) / backingScale_;
    descent_ = static_cast<float>(ftFace_->size->metrics.descender >> 6) / backingScale_;
    atlasData_.resize(atlasW_ * atlasH_ * 4, 0);
    for (uint32_t c = 32; c < 127; ++c) loadGlyph(c);
    if (!uploadAtlas()) return false;
    return true;
}

bool FontAtlas::loadFallbackFace(int fontSize) {
#ifdef _WIN32
    static const char* candidates[] = {
        "C:/Windows/Fonts/seguiemj.ttf",
        "C:/Windows/Fonts/seguisym.ttf",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        if (!std::filesystem::exists(candidates[i])) continue;
        if (!FT_New_Face(ftLib_, candidates[i], 0, &fallbackFace_)) {
            FT_Set_Pixel_Sizes(fallbackFace_, 0, fontSize);
            return true;
        }
    }
#else
    (void)fontSize;
#endif
    return false;
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
    FT_Face face = ftFace_;
    if (fallbackFace_ && FT_Get_Char_Index(ftFace_, cp) == 0 && FT_Get_Char_Index(fallbackFace_, cp) != 0) face = fallbackFace_;
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_COLOR) &&
        FT_Load_Char(face, cp, FT_LOAD_RENDER)) {
        AtlasGlyph blank{};
        blank.advance = glyphs_.count(' ') ? glyphs_[' '].advance : 8.f;
        glyphs_[cp] = blank;
        measureCache_.clear();
        return;
    }
    if (bold_ && !FT_HAS_COLOR(face)) FT_GlyphSlot_Embolden(face->glyph);
    int w = static_cast<int>(face->glyph->bitmap.width);
    int h = static_cast<int>(face->glyph->bitmap.rows);
    AtlasGlyph g;
    g.width = static_cast<float>(w) / backingScale_;
    g.height = static_cast<float>(h) / backingScale_;
    g.bearingX = static_cast<float>(face->glyph->bitmap_left) / backingScale_;
    g.bearingY = static_cast<float>(face->glyph->bitmap_top) / backingScale_;
    g.advance = static_cast<float>(face->glyph->advance.x >> 6) / backingScale_;
    if (w == 0 || h == 0) {
        g.u0 = g.v0 = g.u1 = g.v1 = 0;
        glyphs_[cp] = g;
        measureCache_.clear();
        return;
    }
    int px, py;
    if (!packGlyph(w, h, px, py)) { fprintf(stderr, "Atlas full, cannot pack U+%04X\n", cp); return; }
    const FT_Bitmap& bm = face->glyph->bitmap;
    int pitch = bm.pitch < 0 ? -bm.pitch : bm.pitch;
    for (int row = 0; row < h; ++row) {
        uint8_t* dst = &atlasData_[((py + row) * atlasW_ + px) * 4];
        const uint8_t* src = bm.buffer + row * pitch;
        if (bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
            for (int col = 0; col < w; ++col) {
                uint8_t* out = dst + col * 4;
                out[0] = 255;
                out[1] = 255;
                out[2] = 255;
                out[3] = src[col];
            }
        } else if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
            g.colored = true;
            for (int col = 0; col < w; ++col) {
                const uint8_t* in = src + col * 4;
                uint8_t* out = dst + col * 4;
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = in[3];
            }
        }
    }
    float iw = 1.f / atlasW_, ih = 1.f / atlasH_;
    g.u0 = px * iw;
    g.v0 = py * ih;
    g.u1 = (px + w) * iw;
    g.v1 = (py + h) * ih;
    glyphs_[cp] = g;
    measureCache_.clear();
    dirty_ = true;
}

bool FontAtlas::uploadAtlas() {
    if (!atlasTex_) glGenTextures(1, &atlasTex_);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, atlasW_, atlasH_, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlasData_.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    dirty_ = false;
    return true;
}

const AtlasGlyph& FontAtlas::getGlyph(uint32_t cp) {
    if ((cp < 0x20 && cp != '\t' && cp != '\n' && cp != '\r') || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = '?';
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
            appendGlyphQuad(verts, gl, x0, y0, x1, y1, r, g, b, a);
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

void FontAtlas::drawTextClipped(std::string_view text, float x, float y, float clipLeft, float clipRight, float r, float g, float b, float a) {
    if (text.empty() || clipRight <= clipLeft) return;
    float spaceAdv = getGlyph(' ').advance;
    std::vector<float> verts;
    verts.reserve(visibleGlyphReserve(text.size(), clipLeft, clipRight, static_cast<float>(spaceAdv)) * 6 * 8);
    float cursorX = x;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') { cursorX = x; continue; }
        if (cp == '\t') { cursorX += spaceAdv * 4; if (cursorX > clipRight) break; continue; }
        if (cp == '\r') { cursorX = x; continue; }
        const AtlasGlyph& gl = getGlyph(cp);
        float advance = static_cast<float>(gl.advance);
        float x0 = cursorX + gl.bearingX;
        float x1 = x0 + gl.width;
        if (x1 >= clipLeft && x0 <= clipRight && gl.width > 0 && gl.height > 0) {
            float y0 = y + ascent_ - gl.bearingY;
            float y1 = y0 + gl.height;
            appendGlyphQuad(verts, gl, x0, y0, x1, y1, r, g, b, a);
        }
        cursorX += advance;
        if (cursorX > clipRight + spaceAdv) break;
    }
    if (verts.empty()) return;
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
    glBindVertexArray(0);
}

void FontAtlas::drawTextScaled(std::string_view text, float x, float y, float scale, float r, float g, float b, float a) {
    if (text.empty() || scale <= 0.f) return;
    std::vector<float> verts;
    verts.reserve(text.size() * 6 * 8);
    float cursorX = x;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') { cursorX = x; continue; }
        if (cp == '\t') { cursorX += getGlyph(' ').advance * 4 * scale; continue; }
        if (cp == '\r') { cursorX = x; continue; }
        const AtlasGlyph& gl = getGlyph(cp);
        if (gl.width > 0 && gl.height > 0) {
            float x0 = cursorX + gl.bearingX * scale;
            float y0 = y + (ascent_ - gl.bearingY) * scale;
            float x1 = x0 + gl.width * scale;
            float y1 = y0 + gl.height * scale;
            appendGlyphQuad(verts, gl, x0, y0, x1, y1, r, g, b, a);
        }
        cursorX += gl.advance * scale;
    }
    if (verts.empty()) return;
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, atlasTex_);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
    glBindVertexArray(0);
}

void FontAtlas::drawTextItalic(std::string_view text, float x, float y, float scale, float r, float g, float b, float a) {
    if (text.empty() || scale <= 0.f) return;
    std::vector<float> verts;
    verts.reserve(text.size() * 6 * 8);
    float cursorX = x;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') { cursorX = x; continue; }
        if (cp == '\t') { cursorX += getGlyph(' ').advance * 4 * scale; continue; }
        if (cp == '\r') { cursorX = x; continue; }
        const AtlasGlyph& gl = getGlyph(cp);
        if (gl.width > 0 && gl.height > 0) {
            float x0 = cursorX + gl.bearingX * scale, y0 = y + (ascent_ - gl.bearingY) * scale;
            float x1 = x0 + gl.width * scale, y1 = y0 + gl.height * scale, slant = gl.height * scale * 0.18f;
            float cr = gl.colored ? -1.f : r, cg = gl.colored ? 0.f : g, cb = gl.colored ? 0.f : b;
            verts.insert(verts.end(), {x0 + slant,y0,gl.u0,gl.v0,cr,cg,cb,a, x0,y1,gl.u0,gl.v1,cr,cg,cb,a, x1,y1,gl.u1,gl.v1,cr,cg,cb,a,
                                       x0 + slant,y0,gl.u0,gl.v0,cr,cg,cb,a, x1,y1,gl.u1,gl.v1,cr,cg,cb,a, x1 + slant,y0,gl.u1,gl.v0,cr,cg,cb,a});
        }
        cursorX += gl.advance * scale;
    }
    if (verts.empty()) return;
    glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, atlasTex_); glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8)); glBindVertexArray(0);
}

void FontAtlas::drawTextScaledClipped(std::string_view text, float x, float y, float scale, float clipLeft, float clipRight, float r, float g, float b, float a) {
    if (text.empty() || scale <= 0.f || clipRight <= clipLeft) return;
    float spaceAdv = getGlyph(' ').advance;
    std::vector<float> verts;
    verts.reserve(visibleGlyphReserve(text.size(), clipLeft, clipRight, static_cast<float>(spaceAdv) * scale) * 6 * 8);
    float cursorX = x;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n') { cursorX = x; continue; }
        if (cp == '\t') { cursorX += spaceAdv * 4 * scale; if (cursorX > clipRight) break; continue; }
        if (cp == '\r') { cursorX = x; continue; }
        const AtlasGlyph& gl = getGlyph(cp);
        float advance = gl.advance * scale;
        float x0 = cursorX + gl.bearingX * scale;
        float x1 = x0 + gl.width * scale;
        if (x1 >= clipLeft && x0 <= clipRight && gl.width > 0 && gl.height > 0) {
            float y0 = y + (ascent_ - gl.bearingY) * scale;
            float y1 = y0 + gl.height * scale;
            appendGlyphQuad(verts, gl, x0, y0, x1, y1, r, g, b, a);
        }
        cursorX += advance;
        if (cursorX > clipRight + spaceAdv * scale) break;
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
    measureCache_.clear();
    packCursorX_ = 0; packCursorY_ = 0; packRowH_ = 0; dirty_ = false;
    if (fallbackFace_) { FT_Done_Face(fallbackFace_); fallbackFace_ = nullptr; }
    if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
    if (ftLib_) { FT_Done_FreeType(ftLib_); ftLib_ = nullptr; }
}

float FontAtlas::measureText(std::string_view text) {
    std::string key(text);
    auto cached = measureCache_.find(key);
    if (cached != measureCache_.end()) return cached->second;
    float cx = 0;
    float spaceAdv = 0.f;
    auto it = glyphs_.find(' ');
    if (it != glyphs_.end()) spaceAdv = it->second.advance;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = decodeUtf8(text, i);
        if (cp == '\n' || cp == '\r') break;
        if (cp == '\t') { cx += spaceAdv * 4; continue; }
        cx += getGlyph(cp).advance;
    }
    measureCache_.emplace(std::move(key), cx);
    return cx;
}

FontAtlas& fontAtlas() {
    static FontAtlas atlas;
    return atlas;
}

FontAtlas& uiFontAtlas() {
    static FontAtlas atlas;
    return atlas;
}
