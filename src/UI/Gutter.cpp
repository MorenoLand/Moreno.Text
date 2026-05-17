#include "UI/Gutter.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <cstdio>
#include <string>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

int Gutter::digitCount(size_t n) { int d = 1; while (n >= 10) { n /= 10; ++d; } return d; }

void Gutter::draw(FontAtlas& font, size_t totalLines, size_t currentLine,
                  size_t firstVisibleLine, float originY, float lineStep, float windowH, float titlebarH) {
    int digits = digitCount(totalLines);
    float charW = font.measureText("8");
    width_ = digits * charW + padding_ * 2 + 12.f;
    float borderX = width_;
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
    addRect(0, titlebarH, borderX, windowH, 0.11f, 0.11f, 0.13f, 1.f);
    addRect(borderX - 1, titlebarH, borderX, windowH, 0.08f, 0.08f, 0.10f, 1.f);
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
            std::string num = std::to_string(line + 1);
            float numW = font.measureText(num);
            float nx = borderX - padding_ - numW;
            bool isCurrent = (line == currentLine);
            float brightness = isCurrent ? 0.85f : 0.34f;
            font.drawText(num, nx, y, brightness, brightness, brightness, 1.f);
        }
        y += lineStep;
    }
}
