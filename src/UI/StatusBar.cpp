#include "UI/StatusBar.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <cstdio>

extern GLuint gl_vao();
extern GLuint gl_vbo();

void StatusBar::draw(FontAtlas& font, float windowW, float windowH, float minimapW,
                     size_t line, size_t col, const std::string& syntaxName, bool useTabs, int tabSize, const std::string& branch) {
    float barY = windowH - height_;
    std::vector<float> verts;
    auto addRect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a, x0,y1, 0,0, r,g,b,a, x1,y1, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a, x1,y1, 0,0, r,g,b,a, x1,y0, 0,0, r,g,b,a });
    };
    addRect(0, barY, windowW - minimapW, windowH, 0.15f, 0.15f, 0.18f, 1.f);
    GLRenderer::setDrawMode(2);
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 8));
    glBindVertexArray(0);
    GLRenderer::setDrawMode(0);
    char buf[64];
    snprintf(buf, sizeof(buf), "Line %zu, Col %zu", line + 1, col + 1);
    font.drawText(buf, 12.f, barY + 3.f, 0.7f, 0.7f, 0.7f, 1.f);
    if (!branch.empty()) {
        float branchX = 12.f + font.measureText(buf) + 24.f;
        std::vector<float> icon;
        auto addIconRect = [&](float x0, float y0, float x1, float y1) {
            icon.insert(icon.end(), { x0,y0, 0,0, 0.45f,0.70f,0.55f,1.f, x0,y1, 0,0, 0.45f,0.70f,0.55f,1.f, x1,y1, 0,0, 0.45f,0.70f,0.55f,1.f });
            icon.insert(icon.end(), { x0,y0, 0,0, 0.45f,0.70f,0.55f,1.f, x1,y1, 0,0, 0.45f,0.70f,0.55f,1.f, x1,y0, 0,0, 0.45f,0.70f,0.55f,1.f });
        };
        float ix = branchX, iy = barY + 6.f;
        addIconRect(ix + 4.f, iy + 1.f, ix + 6.f, iy + 12.f);
        addIconRect(ix + 2.f, iy + 3.f, ix + 4.f, iy + 5.f);
        addIconRect(ix + 6.f, iy + 6.f, ix + 8.f, iy + 8.f);
        addIconRect(ix + 1.f, iy + 2.f, ix + 3.f, iy + 4.f);
        addIconRect(ix + 7.f, iy + 5.f, ix + 9.f, iy + 7.f);
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, icon.size() * sizeof(float), icon.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(icon.size() / 8));
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
        font.drawText(branch, branchX + 16.f, barY + 3.f, 0.5f, 0.7f, 0.5f, 1.f);
    }
    // right side: indent mode + syntax name
    char indentBuf[64];
    snprintf(indentBuf, sizeof(indentBuf), useTabs ? "Tab Size: %d" : "Spaces: %d", tabSize);
    float synW = font.measureText(syntaxName);
    float indW = font.measureText(indentBuf);
    float rightEdge = windowW - minimapW - 12.f;
    syntaxLabelX_ = rightEdge - synW;
    syntaxLabelW_ = synW + 6.f;
    indentLabelX_ = syntaxLabelX_ - indW - 24.f;
    indentLabelW_ = indW + 6.f;
    font.drawText(indentBuf, indentLabelX_ + 3.f, barY + 3.f, 0.65f, 0.65f, 0.68f, 1.f);
    font.drawText("|", indentLabelX_ + indW + 10.f, barY + 3.f, 0.3f, 0.3f, 0.33f, 1.f);
    font.drawText(syntaxName, syntaxLabelX_ + 3.f, barY + 3.f, 0.65f, 0.65f, 0.68f, 1.f);
}
