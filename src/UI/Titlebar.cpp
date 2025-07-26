#include "UI/Titlebar.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "icon_data.h"
#include <GL/glew.h>
#include <cstdio>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

void Titlebar::init(int windowWidth) {
    buttons_[0].type = TitlebarButton::MENU;
    buttons_[1].type = TitlebarButton::MINIMIZE;
    buttons_[2].type = TitlebarButton::MAXIMIZE;
    buttons_[3].type = TitlebarButton::CLOSE;
    layout(windowWidth);
    // upload icon as RGBA texture
    if (!iconTex_ && icon_width > 0) {
        glGenTextures(1, &iconTex_);
        glBindTexture(GL_TEXTURE_2D, iconTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon_width, icon_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // premultiply alpha into RGB so blending works correctly
        iconW_ = icon_width;
        iconH_ = icon_height;
    }
}

void Titlebar::layout(int windowWidth) { updateButtonPositions(windowWidth); }

void Titlebar::updateButtonPositions(int ww) {
    float bw = buttonSize_;
    float bh = height_;
    buttons_[3].x = ww - bw; buttons_[3].y = 0; buttons_[3].w = bw; buttons_[3].h = bh;
    buttons_[2].x = ww - bw * 2; buttons_[2].y = 0; buttons_[2].w = bw; buttons_[2].h = bh;
    buttons_[1].x = ww - bw * 3; buttons_[1].y = 0; buttons_[1].w = bw; buttons_[1].h = bh;
    buttons_[0].x = 0; buttons_[0].y = 0; buttons_[0].w = 36; buttons_[0].h = bh;
}

void Titlebar::draw(FontAtlas& font, float r, float g, float b, float a) {
    std::vector<float> verts;
    auto addRect = [&](float x0, float y0, float x1, float y1, float cr, float cg, float cb, float ca) {
        verts.insert(verts.end(), { x0,y0, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x0,y1, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x1,y1, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x0,y0, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x1,y1, 0,0, cr,cg,cb,ca });
        verts.insert(verts.end(), { x1,y0, 0,0, cr,cg,cb,ca });
    };
    // title bar background
    addRect(0, 0, buttons_[1].x, height_, 0.16f, 0.16f, 0.19f, 1.f);
    // buttons (min, max, close)
    for (int i = 1; i < 4; ++i) {
        auto& btn = buttons_[i];
        if (btn.type == TitlebarButton::CLOSE) {
            float br = btn.hovered ? 0.85f : 0.35f;
            addRect(btn.x, btn.y, btn.x + btn.w, btn.y + btn.h, br, btn.hovered ? 0.15f : 0.15f, btn.hovered ? 0.15f : 0.18f, 1.f);
        } else {
            float br = btn.hovered ? 0.28f : 0.2f;
            addRect(btn.x, btn.y, btn.x + btn.w, btn.y + btn.h, br, br, br + 0.03f, 1.f);
        }
    }
    // menu button bg
    auto& menuBtn = buttons_[0];
    float mbr = menuBtn.hovered ? 0.28f : 0.16f;
    addRect(menuBtn.x, menuBtn.y, menuBtn.x + menuBtn.w, menuBtn.y + menuBtn.h, mbr, mbr, mbr + 0.03f, 1.f);
    // flush non-textured quads (solid color mode)
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
    // icon (textured quad — RGBA mode)
    if (iconTex_) {
        GLRenderer::setDrawMode(1);
        float pad = (height_ - iconW_) / 2.f;
        float ix = pad, iy = pad;
        std::vector<float> iv = {
            ix, iy, 0, 0, 1,1,1,1,
            ix, iy + iconH_, 0, 1, 1,1,1,1,
            ix + iconW_, iy + iconH_, 1, 1, 1,1,1,1,
            ix, iy, 0, 0, 1,1,1,1,
            ix + iconW_, iy + iconH_, 1, 1, 1,1,1,1,
            ix + iconW_, iy, 1, 0, 1,1,1,1
        };
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, iv.size() * sizeof(float), iv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, iconTex_);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
    }
    // title text — centered between menu button and window buttons
    float titleLeft = buttons_[0].x + buttons_[0].w + 8.f;
    float titleRight = buttons_[1].x - 8.f;
    float titleMid = (titleLeft + titleRight) / 2.f;
    float titleW = font.measureText(title_);
    float titleX = titleMid - titleW / 2.f;
    if (titleX < titleLeft) titleX = titleLeft;
    font.drawText(title_, titleX, 6.f, 0.7f, 0.7f, 0.7f, 1.f);
    // button symbols
    float mid = height_ / 2.f;
    font.drawText("_", buttons_[1].x + 16.f, mid - 4.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xe2\x96\xa1", buttons_[2].x + 16.f, mid - 8.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xc3\x97", buttons_[3].x + 16.f, mid - 8.f, 1.f, 1.f, 1.f, 1.f);
    // separator line
    verts.clear();
    addRect(0, height_ - 1, buttons_[1].x, height_, 0.1f, 0.1f, 0.12f, 1.f);
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
}

SDL_HitTestResult Titlebar::hitTest(int mx, int my, SDL_Window* window) {
    int ww, wh;
    SDL_GetWindowSize(window, &ww, &wh);
    float re = resizeEdge_;
    if (mx < re && my < re) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (mx >= ww - re && my < re) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (mx < re && my >= wh - re) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (mx >= ww - re && my >= wh - re) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (my < re) return SDL_HITTEST_RESIZE_TOP;
    if (my >= wh - re) return SDL_HITTEST_RESIZE_BOTTOM;
    if (mx < re) return SDL_HITTEST_RESIZE_LEFT;
    if (mx >= ww - re) return SDL_HITTEST_RESIZE_RIGHT;
    if (my < static_cast<int>(height_)) {
        for (auto& btn : buttons_) {
            if (mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x + btn.w)) {
                return SDL_HITTEST_NORMAL;
            }
        }
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

bool Titlebar::handleEvent(const SDL_Event& e, SDL_Window* window) {
    if (e.type == SDL_MOUSEMOTION) {
        int mx = e.motion.x, my = e.motion.y;
        for (auto& btn : buttons_) {
            btn.hovered = mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x + btn.w)
                       && my >= static_cast<int>(btn.y) && my <= static_cast<int>(btn.y + btn.h);
        }
        return false;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        int mx = e.button.x, my = e.button.y;
        if (my < static_cast<int>(height_)) {
            for (auto& btn : buttons_) {
                if (mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x + btn.w)) {
                    switch (btn.type) {
                        case TitlebarButton::CLOSE: { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); return true; }
                        case TitlebarButton::MAXIMIZE:
                            (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) ? SDL_RestoreWindow(window) : SDL_MaximizeWindow(window);
                            return true;
                        case TitlebarButton::MINIMIZE: SDL_MinimizeWindow(window); return true;
                        case TitlebarButton::MENU: return true;
                    }
                }
            }
            if (e.button.clicks == 2) {
                (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) ? SDL_RestoreWindow(window) : SDL_MaximizeWindow(window);
                return true;
            }
        }
    }
    return false;
}
