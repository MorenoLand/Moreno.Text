#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <cstdio>
#include <vector>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

bool FontAtlas::init(const std::string& fontPath, int fontSize) {
    if (FT_Init_FreeType(&ftLib_)) { fprintf(stderr, "FT_Init_FreeType failed\n"); return false; }
    if (FT_New_Face(ftLib_, fontPath.c_str(), 0, &ftFace_)) { fprintf(stderr, "Failed to load font: %s\n", fontPath.c_str()); return false; }
    FT_Set_Pixel_Sizes(ftFace_, 0, fontSize);
    lineHeight_ = static_cast<float>(ftFace_->size->metrics.height >> 6);
    // pre-load ASCII printable range
    for (uint32_t c = 32; c < 127; ++c) loadGlyph(c);
    return true;
}

void FontAtlas::loadGlyph(uint32_t cp) {
    if (glyphs_.count(cp)) return;
    if (FT_Load_Char(ftFace_, cp, FT_LOAD_RENDER)) { fprintf(stderr, "Failed to load glyph U+%04X\n", cp); return; }
    Glyph g;
    g.width = static_cast<int>(ftFace_->glyph->bitmap.width);
    g.height = static_cast<int>(ftFace_->glyph->bitmap.rows);
    g.bearingX = ftFace_->glyph->bitmap_left;
    g.bearingY = ftFace_->glyph->bitmap_top;
    g.advance = static_cast<int>(ftFace_->glyph->advance.x >> 6);
    glGenTextures(1, &g.texID);
    glBindTexture(GL_TEXTURE_2D, g.texID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, g.width, g.height, 0, GL_RED, GL_UNSIGNED_BYTE, ftFace_->glyph->bitmap.buffer);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glyphs_[cp] = g;
}

const Glyph& FontAtlas::getGlyph(uint32_t cp) {
    auto it = glyphs_.find(cp);
    if (it != glyphs_.end()) return it->second;
    loadGlyph(cp);
    return glyphs_[cp];
}

void FontAtlas::drawText(std::string_view text, float x, float y, float r, float g, float b, float a) {
    std::vector<float> verts;
    verts.reserve(text.size() * 6 * 8);
    float cursorX = x;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = static_cast<uint8_t>(text[i]);
        // UTF-8 decode
        if (cp >= 0xF0 && i+3 < text.size()) { cp = (cp&0x07)<<18 | (text[i+1]&0x3F)<<12 | (text[i+2]&0x3F)<<6 | (text[i+3]&0x3F); i+=4; }
        else if (cp >= 0xE0 && i+2 < text.size()) { cp = (cp&0x0F)<<12 | (text[i+1]&0x3F)<<6 | (text[i+2]&0x3F); i+=3; }
        else if (cp >= 0xC0 && i+1 < text.size()) { cp = (cp&0x1F)<<6 | (text[i+1]&0x3F); i+=2; }
        else { ++i; }
        if (cp == '\r' || cp == '\t') { cursorX += getGlyph(' ').advance; continue; }
        const Glyph& gl = getGlyph(cp);
        float x0 = cursorX + gl.bearingX;
        float y0 = y + gl.bearingY;
        float x1 = x0 + gl.width;
        float y1 = y0 + gl.height;
        float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
        // tri 1
        verts.insert(verts.end(), { x0,y0, u0,v0, r,g,b,a });
        verts.insert(verts.end(), { x0,y1, u0,v1, r,g,b,a });
        verts.insert(verts.end(), { x1,y1, u1,v1, r,g,b,a });
        // tri 2
        verts.insert(verts.end(), { x0,y0, u0,v0, r,g,b,a });
        verts.insert(verts.end(), { x1,y1, u1,v1, r,g,b,a });
        verts.insert(verts.end(), { x1,y0, u1,v0, r,g,b,a });
        cursorX += gl.advance;
    }
    if (verts.empty()) return;
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, 0);
    // bind each glyph's texture individually for now — atlas approach in next phase
    // for phase 1 we batch by iterating glyphs again with individual draw calls
    // actually let's just draw all with a placeholder — we'll use glyph textures per-batch
    // Simplified: single draw per text call using the last glyph texture as a test
    // Proper fix: draw per-glyph
    size_t vertIdx = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp = static_cast<uint8_t>(text[i]);
        if (cp >= 0xF0 && i+3 < text.size()) { i+=4; }
        else if (cp >= 0xE0 && i+2 < text.size()) { i+=3; }
        else if (cp >= 0xC0 && i+1 < text.size()) { i+=2; }
        else { ++i; }
        if (cp == '\r' || cp == '\t') { vertIdx += 6*8; continue; }
        const Glyph& gl = getGlyph(cp);
        glBindTexture(GL_TEXTURE_2D, gl.texID);
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(vertIdx / 8), 6);
        vertIdx += 6 * 8;
    }
    glBindVertexArray(0);
}

void FontAtlas::destroy() {
    for (auto& [cp, g] : glyphs_) if (g.texID) glDeleteTextures(1, &g.texID);
    glyphs_.clear();
    if (ftFace_) { FT_Done_Face(ftFace_); ftFace_ = nullptr; }
    if (ftLib_) { FT_Done_FreeType(ftLib_); ftLib_ = nullptr; }
}

FontAtlas& fontAtlas() {
    static FontAtlas atlas;
    return atlas;
}
