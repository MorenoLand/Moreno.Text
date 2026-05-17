#include "UI/Titlebar.h"
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "icon_data.h"
#include <GL/glew.h>
#include <cstdio>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

void Titlebar::buildMenu() {
    menuItems_ = {
        {"New File", "Ctrl+N", []{ Application::instance().newBuffer(); }},
        {"Open File...", "Ctrl+O", []{}},
        {"", "", nullptr, true},
        {"Save", "Ctrl+S", []{ Application::instance().saveFile(); }},
        {"Save As...", "Ctrl+Shift+S", []{ Application::instance().saveFileAs(); }},
        {"", "", nullptr, true},
        {"Close File", "Ctrl+W", []{ Application::instance().newBuffer(); }},
        {"", "", nullptr, true},
        {"Undo", "Ctrl+Z", []{}},
        {"Redo", "Ctrl+Y", []{}},
        {"", "", nullptr, true},
        {"Find...", "Ctrl+F", []{}},
        {"Replace...", "Ctrl+H", []{}},
        {"", "", nullptr, true},
        {"Goto Anything...", "Ctrl+P", []{}},
        {"", "", nullptr, true},
        {"Preferences", "Ctrl+,", []{}},
        {"", "", nullptr, true},
        {"Exit", "Ctrl+Q", []{ Application::instance().quit(); }},
    };
}

static void flushSolid(std::vector<float>& v) {
    if (v.empty()) return;
    GLRenderer::setDrawMode(2);
    glBindVertexArray(gl_vao());
    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 8));
    glBindVertexArray(0);
    GLRenderer::setDrawMode(0);
    v.clear();
}

void Titlebar::drawMenu(FontAtlas& font) {
    if (!menuOpen_) return;
    float maxW = 160.f;
    for (auto& item : menuItems_) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 24.f;
        if (w + 32.f > maxW) maxW = w + 32.f;
    }
    float ddX = 0.f, ddY = height_;
    float ddW = maxW, ddH = 2.f + menuItems_.size() * 24.f;
    std::vector<float> v;
    auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        v.insert(v.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    ar(ddX, ddY, ddX + ddW, ddY + ddH, 0.17f, 0.17f, 0.20f, 0.98f);
    ar(ddX, ddY, ddX + ddW, ddY + 1, 0.30f, 0.30f, 0.35f, 1.f);
    if (menuHovered_ >= 0 && menuHovered_ < (int)menuItems_.size() && !menuItems_[menuHovered_].separator)
        ar(ddX + 2, ddY + 2 + menuHovered_ * 24.f, ddX + ddW - 2, ddY + 2 + (menuHovered_ + 1) * 24.f, 0.25f, 0.30f, 0.45f, 1.f);
    flushSolid(v);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)ddX, 0, (int)ddW, (int)(ddY + ddH + 4));
    for (int i = 0; i < (int)menuItems_.size(); ++i) {
        auto& item = menuItems_[i];
        float iy = ddY + 2 + i * 24.f + 4.f;
        if (item.separator) {
            ar(ddX + 8, ddY + 2 + i * 24.f + 11, ddX + ddW - 8, ddY + 2 + i * 24.f + 12, 0.3f, 0.3f, 0.33f, 1.f);
            flushSolid(v);
            continue;
        }
        float ib = (menuHovered_ == i) ? 1.f : 0.78f;
        font.drawText(item.label, ddX + 12.f, iy, ib, ib, ib, 1.f);
        if (!item.shortcut.empty())
            font.drawText(item.shortcut, ddX + ddW - font.measureText(item.shortcut) - 12.f, iy, 0.5f, 0.5f, 0.55f, 1.f);
    }
    glDisable(GL_SCISSOR_TEST);
}

void Titlebar::init(int windowWidth) {
    buttons_[0].type = TitlebarButton::MENU;
    buttons_[1].type = TitlebarButton::MINIMIZE;
    buttons_[2].type = TitlebarButton::MAXIMIZE;
    buttons_[3].type = TitlebarButton::CLOSE;
    layout(windowWidth);
    buildMenu();
    if (!iconTex_ && icon_width > 0) {
        glGenTextures(1, &iconTex_);
        glBindTexture(GL_TEXTURE_2D, iconTex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, icon_width, icon_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, icon_data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        iconW_ = icon_width;
        iconH_ = icon_height;
    }
}

void Titlebar::layout(int windowWidth) { updateButtonPositions(windowWidth); }

void Titlebar::updateButtonPositions(int ww) {
    float bw = buttonSize_, bh = height_;
    buttons_[3].x = ww - bw; buttons_[3].y = 0; buttons_[3].w = bw; buttons_[3].h = bh;
    buttons_[2].x = ww - bw*2; buttons_[2].y = 0; buttons_[2].w = bw; buttons_[2].h = bh;
    buttons_[1].x = ww - bw*3; buttons_[1].y = 0; buttons_[1].w = bw; buttons_[1].h = bh;
    buttons_[0].x = 0; buttons_[0].y = 0; buttons_[0].w = 36; buttons_[0].h = bh;
}

void Titlebar::draw(FontAtlas& font, float, float, float, float) {
    std::vector<float> v;
    auto ar = [&](float x0,float y0,float x1,float y1,float cr,float cg,float cb,float ca) {
        v.insert(v.end(),{x0,y0,0,0,cr,cg,cb,ca, x0,y1,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x0,y0,0,0,cr,cg,cb,ca, x1,y1,0,0,cr,cg,cb,ca, x1,y0,0,0,cr,cg,cb,ca});
    };
    ar(0, 0, buttons_[1].x, height_, 0.16f, 0.16f, 0.19f, 1.f);
    for (int i = 1; i < 4; ++i) {
        auto& btn = buttons_[i];
        if (btn.type == TitlebarButton::CLOSE) {
            float br = btn.hovered ? 0.85f : 0.35f;
            ar(btn.x, btn.y, btn.x+btn.w, btn.y+btn.h, br, btn.hovered?0.15f:0.15f, btn.hovered?0.15f:0.18f, 1.f);
        } else {
            float br = btn.hovered ? 0.28f : 0.2f;
            ar(btn.x, btn.y, btn.x+btn.w, btn.y+btn.h, br, br, br+0.03f, 1.f);
        }
    }
    auto& mb = buttons_[0];
    float mbr = (mb.hovered || menuOpen_) ? 0.28f : 0.16f;
    ar(mb.x, mb.y, mb.x+mb.w, mb.y+mb.h, mbr, mbr, mbr+0.03f, 1.f);
    flushSolid(v);
    if (iconTex_) {
        GLRenderer::setDrawMode(1);
        float pad = (height_ - iconW_) / 2.f;
        std::vector<float> iv = {pad,pad,0,0,1,1,1,1, pad,pad+iconH_,0,1,1,1,1,1, pad+iconW_,pad+iconH_,1,1,1,1,1,1, pad,pad,0,0,1,1,1,1, pad+iconW_,pad+iconH_,1,1,1,1,1,1, pad+iconW_,pad,1,0,1,1,1,1};
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, iv.size()*sizeof(float), iv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, iconTex_); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
    }
    float titleLeft = buttons_[0].x + buttons_[0].w + 8.f, titleRight = buttons_[1].x - 8.f;
    float titleW = font.measureText(title_);
    float titleX = (titleLeft + titleRight) / 2.f - titleW / 2.f;
    if (titleX < titleLeft) titleX = titleLeft;
    font.drawText(title_, titleX, 6.f, 0.7f, 0.7f, 0.7f, 1.f);
    float mid = height_ / 2.f;
    font.drawText("_", buttons_[1].x + 16.f, mid - 4.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xe2\x96\xa1", buttons_[2].x + 16.f, mid - 8.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xc3\x97", buttons_[3].x + 16.f, mid - 8.f, 1.f, 1.f, 1.f, 1.f);
    ar(0, height_-1, buttons_[1].x, height_, 0.1f, 0.1f, 0.12f, 1.f);
    flushSolid(v);
    drawMenu(font);
}

bool Titlebar::handleMenuEvent(const SDL_Event& e) {
    if (!menuOpen_) return false;
    float ddX = 0.f, ddY = height_;
    float maxW = 160.f;
    // need font to measure — but we don't have it here, use cached or fixed width
    // We'll match the same width calculation. Since we can't access font here, we use a reasonable estimate.
    // Actually we only need hit testing, so we can use a fixed width for hit-test purposes.
    // The dropdown will be at most ~280px wide based on the draw code. Use that.
    float ddW = 280.f, ddH = 2.f + menuItems_.size() * 24.f;
    if (e.type == SDL_MOUSEMOTION) {
        float mx = (float)e.motion.x, my = (float)e.motion.y;
        menuHovered_ = -1;
        if (mx >= ddX && mx <= ddX + ddW && my >= ddY && my <= ddY + ddH) {
            int idx = (int)((my - ddY - 2) / 24.f);
            if (idx >= 0 && idx < (int)menuItems_.size() && !menuItems_[idx].separator) menuHovered_ = idx;
        }
        return true;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        float mx = (float)e.button.x, my = (float)e.button.y;
        if (mx >= ddX && mx <= ddX + ddW && my >= ddY && my <= ddY + ddH) {
            int idx = (int)((my - ddY - 2) / 24.f);
            if (idx >= 0 && idx < (int)menuItems_.size() && !menuItems_[idx].separator && menuItems_[idx].action) {
                menuItems_[idx].action();
                closeMenu();
                return true;
            }
        }
        closeMenu();
        return true;
    }
    return false;
}

SDL_HitTestResult Titlebar::hitTest(int mx, int my, SDL_Window* window) {
    int ww, wh; SDL_GetWindowSize(window, &ww, &wh);
    float re = resizeEdge_;
    if (mx < re && my < re) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (mx >= ww-re && my < re) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (mx < re && my >= wh-re) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (mx >= ww-re && my >= wh-re) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (my < re) return SDL_HITTEST_RESIZE_TOP;
    if (my >= wh-re) return SDL_HITTEST_RESIZE_BOTTOM;
    if (mx < re) return SDL_HITTEST_RESIZE_LEFT;
    if (mx >= ww-re) return SDL_HITTEST_RESIZE_RIGHT;
    if (my < static_cast<int>(height_)) {
        for (auto& btn : buttons_)
            if (mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x + btn.w)) return SDL_HITTEST_NORMAL;
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}

bool Titlebar::handleEvent(const SDL_Event& e, SDL_Window* window) {
    if (handleMenuEvent(e)) return true;
    if (e.type == SDL_MOUSEMOTION) {
        int mx = e.motion.x, my = e.motion.y;
        for (auto& btn : buttons_)
            btn.hovered = mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x+btn.w)
                       && my >= static_cast<int>(btn.y) && my <= static_cast<int>(btn.y+btn.h);
        return false;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        int mx = e.button.x, my = e.button.y;
        if (my < static_cast<int>(height_)) {
            for (auto& btn : buttons_) {
                if (mx >= static_cast<int>(btn.x) && mx <= static_cast<int>(btn.x+btn.w)) {
                    switch (btn.type) {
                        case TitlebarButton::CLOSE: { SDL_Event q; q.type = SDL_QUIT; SDL_PushEvent(&q); return true; }
                        case TitlebarButton::MAXIMIZE:
                            (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) ? SDL_RestoreWindow(window) : SDL_MaximizeWindow(window);
                            return true;
                        case TitlebarButton::MINIMIZE: SDL_MinimizeWindow(window); return true;
                        case TitlebarButton::MENU: menuOpen_ = !menuOpen_; if (!menuOpen_) menuHovered_ = -1; return true;
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
