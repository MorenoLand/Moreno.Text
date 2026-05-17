#include "UI/StatusBar.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <cstdio>

extern GLuint gl_vao();
extern GLuint gl_vbo();

static void addStatusRect(std::vector<float>& verts, float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
    verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a, x0,y1, 0,0, r,g,b,a, x1,y1, 0,0, r,g,b,a });
    verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a, x1,y1, 0,0, r,g,b,a, x1,y0, 0,0, r,g,b,a });
}

void StatusBar::appendSolidRects(FontAtlas& font, std::vector<float>& verts, float windowW, float windowH,
                                 size_t line, size_t col, const std::string& branch) {
    float barY = windowH - height_;
    addStatusRect(verts, 0, barY, windowW, windowH, 0.15f, 0.15f, 0.18f, 1.f);

    if (!branch.empty()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Line %zu, Col %zu", line + 1, col + 1);
        float ix = 12.f + font.measureText(buf) + 24.f;
        float iy = barY + 6.f;
        addStatusRect(verts, ix + 4.f, iy + 1.f, ix + 6.f, iy + 12.f, 0.45f, 0.70f, 0.55f, 1.f);
        addStatusRect(verts, ix + 2.f, iy + 3.f, ix + 4.f, iy + 5.f, 0.45f, 0.70f, 0.55f, 1.f);
        addStatusRect(verts, ix + 6.f, iy + 6.f, ix + 8.f, iy + 8.f, 0.45f, 0.70f, 0.55f, 1.f);
        addStatusRect(verts, ix + 1.f, iy + 2.f, ix + 3.f, iy + 4.f, 0.45f, 0.70f, 0.55f, 1.f);
        addStatusRect(verts, ix + 7.f, iy + 5.f, ix + 9.f, iy + 7.f, 0.45f, 0.70f, 0.55f, 1.f);
    }
}

void StatusBar::drawText(FontAtlas& font, float windowW, float windowH,
                         size_t line, size_t col, const std::string& syntaxName, bool useTabs, int tabSize, const std::string& branch) {
    float barY = windowH - height_;
    char buf[64];
    snprintf(buf, sizeof(buf), "Line %zu, Col %zu", line + 1, col + 1);
    font.drawText(buf, 12.f, barY + 3.f, 0.7f, 0.7f, 0.7f, 1.f);

    if (!branch.empty()) {
        float branchX = 12.f + font.measureText(buf) + 24.f;
        font.drawText(branch, branchX + 16.f, barY + 3.f, 0.5f, 0.7f, 0.5f, 1.f);
    }

    char indentBuf[64];
    snprintf(indentBuf, sizeof(indentBuf), useTabs ? "Tab Size: %d" : "Spaces: %d", tabSize);
    float synW = font.measureText(syntaxName);
    float indW = font.measureText(indentBuf);
    float rightEdge = windowW - 12.f;
    syntaxLabelX_ = rightEdge - synW;
    syntaxLabelW_ = synW + 6.f;
    indentLabelX_ = syntaxLabelX_ - indW - 24.f;
    indentLabelW_ = indW + 6.f;
    font.drawText(indentBuf, indentLabelX_ + 3.f, barY + 3.f, 0.65f, 0.65f, 0.68f, 1.f);
    font.drawText("|", indentLabelX_ + indW + 10.f, barY + 3.f, 0.3f, 0.3f, 0.33f, 1.f);
    font.drawText(syntaxName, syntaxLabelX_ + 3.f, barY + 3.f, 0.65f, 0.65f, 0.68f, 1.f);
}

void StatusBar::draw(FontAtlas& font, float windowW, float windowH, float,
                     size_t line, size_t col, const std::string& syntaxName, bool useTabs, int tabSize, const std::string& branch) {
    std::vector<float> verts;
    appendSolidRects(font, verts, windowW, windowH, line, col, branch);
    if (!verts.empty()) {
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 8));
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
    }
    drawText(font, windowW, windowH, line, col, syntaxName, useTabs, tabSize, branch);
}
