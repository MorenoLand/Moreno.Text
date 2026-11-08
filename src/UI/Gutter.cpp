#include "UI/Gutter.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "Theme/ThemeEngine.h"
#include <GL/glew.h>
#include <cstdio>
#include <string>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

int Gutter::digitCount(size_t n) { int d = 1; while (n >= 10) { n /= 10; ++d; } return d; }

void Gutter::draw(FontAtlas& font, size_t totalLines, size_t currentLine,
                  size_t firstVisibleLine, float originX, float originY, float lineStep, float windowH, float titlebarH) {
    int digits = digitCount(totalLines);
    float charW = font.measureText("8");
    width_ = digits * charW + padding_ * 2 + 12.f;
    float borderX = originX + width_;
    // gutter background + 1px right border (solid color)
    std::vector<float> verts;
    auto addRect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x0,y1, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x1,y1, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x1,y1, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x1,y0, 0,0, r,g,b,a });
    };
    auto& themeEngine = ThemeEngine::instance();
    ThemeColor gbg = themeEngine.gutterColor(), gfg = themeEngine.gutterFg(), cur = themeEngine.fgColor();
    float lum = (gbg.r + gbg.g + gbg.b) / 3.f, edge = lum < 0.5f ? -0.03f : -0.08f;
    addRect(originX, titlebarH, borderX, windowH, gbg.r, gbg.g, gbg.b, 1.f);
    addRect(borderX - 1, titlebarH, borderX, windowH, gbg.r + edge, gbg.g + edge, gbg.b + edge, 1.f);
    if (!verts.empty()) {
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
    }
    // line numbers (font atlas mode — drawText handles mode internally)
    float y = originY;
    for (size_t line = firstVisibleLine; line < totalLines; ++line) {
        if (y - font.ascent() > windowH) break;
        if (y + font.descent() >= titlebarH) {
            size_t shown = line + 1;
            if (relative_ && line != currentLine) shown = line > currentLine ? line - currentLine : currentLine - line;
            std::string num = std::to_string(shown);
            float numW = font.measureText(num);
            float nx = borderX - padding_ - numW;
            bool isCurrent = (line == currentLine);
            ThemeColor nc = isCurrent ? cur : gfg;
            font.drawText(num, nx, y, nc.r, nc.g, nc.b, 1.f);
        }
        y += lineStep;
    }
}
