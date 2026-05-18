#include "UI/Minimap.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "Syntax/SyntaxHighlighter.h"
#include <GL/glew.h>
#include <string>
#include <string_view>
#include <vector>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

void Minimap::draw(FontAtlas& font, SyntaxHighlighter& syntax, const std::string& buffer,
                   float originX, float originY, float windowH, float titlebarH,
                   float gutterW, float lineStep, float scrollY, float maxScrollParam, bool hovered) {
    float top = titlebarH;
    float statusH = 22.f;
    float bottom = windowH - statusH;
    float minimapH = bottom - top;
    size_t lineCount = 1;
    for (char c : buffer) if (c == '\n') ++lineCount;
    float scale = 0.22f;
    float miniLineStep = lineStep * scale;
    float contentH = lineCount * lineStep;
    float viewH = minimapH;
    float maxScroll = contentH > viewH ? contentH - viewH : 0.f;
    std::vector<float> verts;
    auto addRect = [&](float x0, float y0, float x1, float y1, float cr, float cg, float cb, float ca) {
        verts.insert(verts.end(), {x0,y0,0,0,cr,cg,cb,ca, x0,y1,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x0,y0,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x1,y0,0,0,cr,cg,cb,ca});
    };
    auto flushSolid = [&] {
        if (verts.empty()) return;
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
        glBindVertexArray(0); GLRenderer::setDrawMode(0);
        verts.clear();
    };
    addRect(originX - 1, top, originX, bottom, 0.09f, 0.09f, 0.11f, 1.f);
    addRect(originX, top, originX + width_, bottom, 0.13f, 0.13f, 0.15f, 1.f);
    flushSolid();
    float y = top + 2.f;
    float miniTextX = originX + 4.f;
    size_t lineStart = 0;
    while (lineStart <= buffer.size()) {
        size_t lineEnd = buffer.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = buffer.size();
        if (y > bottom) break;
        if (y + miniLineStep > top) {
            std::string_view line(buffer.data() + lineStart, lineEnd - lineStart);
            auto tokens = syntax.highlightLine(line, lineStart);
            if (tokens.empty()) {
                auto& c = syntax.scopeColor(0);
                font.drawTextScaled(line, miniTextX, y, scale, c.r, c.g, c.b, 0.78f);
            } else {
                float cx = miniTextX;
                size_t prevEnd = 0;
                for (auto& tok : tokens) {
                    if (tok.start > lineStart + prevEnd) {
                        auto& c = syntax.scopeColor(0);
                        std::string_view gap(buffer.data() + lineStart + prevEnd, tok.start - lineStart - prevEnd);
                        font.drawTextScaled(gap, cx, y, scale, c.r, c.g, c.b, 0.78f);
                        cx += font.measureText(gap) * scale;
                    }
                    auto& c = syntax.scopeColor(tok.scope);
                    std::string_view tokText(buffer.data() + tok.start, tok.length);
                    font.drawTextScaled(tokText, cx, y, scale, c.r, c.g, c.b, 0.78f);
                    cx += font.measureText(tokText) * scale;
                    prevEnd = (tok.start - lineStart) + tok.length;
                    if (cx > originX + width_ - 4.f) break;
                }
                if (prevEnd < lineEnd - lineStart && cx <= originX + width_ - 4.f) {
                    auto& c = syntax.scopeColor(0);
                    std::string_view rest(buffer.data() + lineStart + prevEnd, lineEnd - lineStart - prevEnd);
                    font.drawTextScaled(rest, cx, y, scale, c.r, c.g, c.b, 0.78f);
                }
            }
        }
        y += miniLineStep;
        lineStart = lineEnd + 1;
    }
    // fixed-size viewport ~15% of minimap height
    {
        float vpH = minimapH * 0.15f;
        float contentH = lineCount * lineStep;
        float maxScroll = maxScrollParam;
        float vpTop = top + (maxScroll > 0.f ? (scrollY / maxScroll) * (minimapH - vpH) : 0.f);
        if (vpTop < top) vpTop = top;
        if (vpTop + vpH > bottom) vpTop = bottom - vpH;
        float a = hoverFade_;
        addRect(originX, vpTop, originX + width_, vpTop + vpH, 0.267f, 0.267f, 0.267f, a);
        addRect(originX, vpTop, originX + width_, vpTop + 1, 0.34f, 0.34f, 0.34f, a);
        addRect(originX, vpTop + vpH - 1, originX + width_, vpTop + vpH, 0.34f, 0.34f, 0.34f, a);
    }
    flushSolid();
}

bool Minimap::handleClick(float mx, float my, float& scrollRatio) {
    scrollRatio = 0.f;
    return false;
}

void Minimap::updateHoverFade(float dt) {
    float target = mouseOver_ ? 0.376f : 0.078f;
    float speed = 1.0f / 0.15f;
    if (hoverFade_ < target) hoverFade_ = std::min(target, hoverFade_ + speed * dt);
    else if (hoverFade_ > target) hoverFade_ = std::max(target, hoverFade_ - speed * dt);
}
