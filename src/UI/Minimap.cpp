#include "UI/Minimap.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <cstdio>
#include <string>
#include <string_view>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

void Minimap::draw(FontAtlas& font, const std::string& buffer,
                   float originX, float originY, float windowH, float titlebarH,
                   float gutterW, float lineStep) {
    float top = titlebarH;
    std::vector<float> verts;
    auto addRect = [&](float x0, float y0, float x1, float y1, float cr, float cg, float cb, float ca) {
        verts.insert(verts.end(), { x0,y0, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x0,y1, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x1,y1, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x0,y0, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x1,y1, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x1,y0, 0,0, cr,cg,cb,ca });
    };
    auto flushQuads = [&]() {
        if (verts.empty()) return;
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
        verts.clear();
    };
    // 1px left border + background
    addRect(originX - 1, top, originX, windowH, 0.08f, 0.08f, 0.10f, 1.f);
    addRect(originX, top, originX + width_, windowH, 0.13f, 0.13f, 0.15f, 1.f);
    // scaled-down line blocks
    float miniLineStep = lineStep * scale_;
    float y = top + 2.f;
    float miniTextX = originX + 4.f;
    size_t lineStart = 0;
    while (lineStart <= buffer.size()) {
        size_t lineEnd = buffer.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = buffer.size();
        if (y > windowH) break;
        std::string_view line(buffer.data() + lineStart, lineEnd - lineStart);
        float lineW = font.measureText(line) * scale_;
        if (lineW > 0) {
            float blockH = miniLineStep * 0.6f;
            float blockY = y + miniLineStep * 0.2f;
            float blockW = (lineW < width_ - 8.f) ? lineW : width_ - 8.f;
            addRect(miniTextX, blockY, miniTextX + blockW, blockY + blockH, 0.45f, 0.45f, 0.50f, 0.5f);
        }
        y += miniLineStep;
        lineStart = lineEnd + 1;
    }
    // viewport highlight
    float visibleLines = (windowH - originY) / lineStep;
    float vpH = visibleLines * miniLineStep;
    addRect(originX, top, originX + width_, top + vpH, 0.2f, 0.2f, 0.25f, 0.25f);
    flushQuads();
}

bool Minimap::handleClick(float mx, float my, float& scrollRatio) {
    scrollRatio = 0.f;
    return false;
}
