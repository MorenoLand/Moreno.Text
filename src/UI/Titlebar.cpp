#include "UI/Titlebar.h"
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "icon_data.h"
#include <GL/glew.h>
#include <algorithm>
#include <cstdio>

extern GLuint gl_shaderProgram();
extern GLuint gl_vao();
extern GLuint gl_vbo();

void Titlebar::buildMenu() {
    menuItems_ = {
        {"File", ">", nullptr, false, 0},
        {"Edit", ">", nullptr, false, 1},
        {"Selection", ">", nullptr, false, 2},
        {"Find", ">", nullptr, false, 3},
        {"View", ">", nullptr, false, 4},
        {"Goto", ">", nullptr, false, 5},
        {"Tools", ">", nullptr, false, 6},
        {"Preferences", ">", nullptr, false, 7},
    };
    submenus_ = {
        {{"New File","Ctrl+N",[]{ Application::instance().newBuffer(); }},{"Open File...","Ctrl+O",[]{ Application::instance().openFileDialog(); }},{"Open Folder...","",[]{ Application::instance().openFolderDialog(); }},{"Open Recent",">",nullptr},{"","",nullptr,true},{"Save","Ctrl+S",[]{ Application::instance().saveFile(); }},{"Save As...","Ctrl+Shift+S",[]{ Application::instance().saveFileAs(); }},{"Save All","",[]{ Application::instance().saveAll(); }},{"","",nullptr,true},{"Close File","Ctrl+W",[]{ Application::instance().closeCurrentTab(); }},{"Reopen Closed Tab","Ctrl+Shift+T",[]{ Application::instance().reopenClosedTab(); }},{"Revert File","",[]{ Application::instance().revertFile(); }},{"","",nullptr,true},{"Exit","Ctrl+Q",[]{ Application::instance().quit(); }}},
        {{"Undo","Ctrl+Z",[]{ Application::instance().commandUndo(); }},{"Redo","Ctrl+Y",[]{ Application::instance().commandRedo(); }},{"","",nullptr,true},{"Cut","Ctrl+X",[]{ Application::instance().commandCut(); }},{"Copy","Ctrl+C",[]{ Application::instance().commandCopy(); }},{"Paste","Ctrl+V",[]{ Application::instance().commandPaste(); }},{"Paste and Indent","Ctrl+Shift+V",[]{ Application::instance().commandPasteAndIndent(); }},{"","",nullptr,true},{"Select All","Ctrl+A",[]{ Application::instance().commandSelectAll(); }},{"","",nullptr,true},{"Indent Line","Ctrl+]",nullptr},{"Unindent Line","Ctrl+[",nullptr},{"","",nullptr,true},{"Swap Line Up","Ctrl+Shift+Up",[]{ Application::instance().swapLineUp(); }},{"Swap Line Down","Ctrl+Shift+Down",[]{ Application::instance().swapLineDown(); }},{"Duplicate Line","Ctrl+Shift+D",[]{ Application::instance().duplicateLine(); }},{"Delete Line","Ctrl+Shift+K",[]{ Application::instance().deleteLine(); }},{"Join Lines","Ctrl+Shift+J",[]{ Application::instance().joinLines(); }},{"","",nullptr,true},{"Toggle Line Comment","Ctrl+/",[]{ Application::instance().toggleLineComment(); }},{"Toggle Block Comment","Ctrl+Shift+/",[]{ Application::instance().toggleBlockComment(); }},{"","",nullptr,true},{"Upper Case","Ctrl+K Ctrl+U",[]{ Application::instance().convertCaseUpper(); }},{"Lower Case","Ctrl+K Ctrl+L",[]{ Application::instance().convertCaseLower(); }},{"Title Case","",[]{ Application::instance().convertCaseTitle(); }},{"Swap Case","",[]{ Application::instance().convertCaseSwap(); }}},
        {{"Select All","Ctrl+A",[]{ Application::instance().commandSelectAll(); }},{"Expand Selection to Line","Ctrl+L",nullptr},{"Expand Selection to Word","Ctrl+D",nullptr},{"Expand Selection to Brackets","Ctrl+Shift+M",nullptr},{"Expand Selection to Indentation","",nullptr},{"Expand Selection to Scope","Ctrl+Shift+Space",nullptr},{"","",nullptr,true},{"Split into Lines","Ctrl+Shift+L",nullptr},{"Single Selection","Escape",nullptr}},
        {{"Find...","Ctrl+F",[]{ Application::instance().commandFind(); }},{"Find Next","F3",nullptr},{"Find Previous","Shift+F3",nullptr},{"Find All","Alt+F3",nullptr},{"","",nullptr,true},{"Replace...","Ctrl+H",[]{ Application::instance().commandReplace(); }},{"","",nullptr,true},{"Find in Files...","Ctrl+Shift+F",nullptr}},
        {{"Toggle Minimap","",[]{ Application::instance().toggleMinimap(); }},{"Hide Tabs","",nullptr},{"Hide Status Bar","",nullptr},{"Toggle Side Bar","Ctrl+K Ctrl+B",[]{ Application::instance().toggleSidebar(); }},{"","",nullptr,true},{"Word Wrap","Alt+Z",[]{ Application::instance().toggleWordWrap(); }},{"","",nullptr,true},{"Enter Full Screen","F11",[]{ Application::instance().toggleFullscreen(); }},{"Enter Distraction Free Mode","Shift+F11",nullptr},{"","",nullptr,true},{"Syntax",">",nullptr},{"Indentation",">",nullptr}},
        {{"Goto Anything...","Ctrl+P",[]{ Application::instance().commandGotoAnything(); }},{"Goto Symbol...","Ctrl+R",nullptr},{"Goto Line...","Ctrl+G",nullptr},{"Goto Word...","Ctrl+;",nullptr},{"","",nullptr,true},{"Jump Back","Alt+-",nullptr},{"Jump Forward","Alt+Shift+-",nullptr},{"","",nullptr,true},{"Next Bookmark","F2",nullptr},{"Previous Bookmark","Shift+F2",nullptr},{"Toggle Bookmark","Ctrl+F2",nullptr},{"Clear All Bookmarks","Ctrl+Shift+F2",nullptr}},
        {{"Command Palette...","Ctrl+Shift+P",[]{ Application::instance().commandPalette(); }},{"","",nullptr,true},{"Record Macro","",[]{}},{"Playback Macro","",[]{}}},
        {{"Settings","",[]{ Application::instance().openSettingsFile(); }},{"Key Bindings","",[]{}},{"Color Scheme",">",[]{}},{"Font",">",[]{}},{"Theme",">",[]{}}},
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

void Titlebar::getMenuBounds(float& x, float& y, float& w, float& h) const {
    x = 0.f;
    y = height_;
    float itemH = 24.f;
    w = 150.f;
    h = 2.f + static_cast<float>(menuItems_.size()) * itemH;
    if (submenuOpen_ >= 0 && submenuOpen_ < static_cast<int>(submenus_.size())) {
        w = 480.f;
        float sy = 2.f + static_cast<float>(submenuOpen_) * itemH;
        float sh = sy + 2.f + static_cast<float>(submenus_[submenuOpen_].size()) * itemH;
        if (sh > h) h = sh;
    }
}

void Titlebar::getMenuPopupBounds(FontAtlas& font, float& x, float& y, float& w, float& h) const {
    SDL_Rect mainRect{}, submenuRect{}; bool hasSubmenu = false;
    getMenuPanelRects(font, 32000, 32000, mainRect, submenuRect, hasSubmenu);
    int minX = mainRect.x, minY = mainRect.y, maxX = mainRect.x + mainRect.w, maxY = mainRect.y + mainRect.h;
    if (hasSubmenu) {
        minX = std::min(minX, submenuRect.x);
        minY = std::min(minY, submenuRect.y);
        maxX = std::max(maxX, submenuRect.x + submenuRect.w);
        maxY = std::max(maxY, submenuRect.y + submenuRect.h);
    }
    x = static_cast<float>(minX) - 3.f;
    y = height_ + static_cast<float>(minY) - 3.f;
    w = static_cast<float>(maxX - minX) + 6.f;
    h = static_cast<float>(maxY - minY) + 6.f;
}

void Titlebar::getMenuPanelRects(FontAtlas& font, int windowW, int windowH, SDL_Rect& mainRect, SDL_Rect& submenuRect, bool& hasSubmenu) const {
    float maxW = 140.f;
    for (auto& item : menuItems_) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 24.f;
        if (w + 32.f > maxW) maxW = w + 32.f;
    }
    float itemH = 24.f;
    float ddW = maxW, ddH = 2.f + static_cast<float>(menuItems_.size()) * itemH;
    mainRect = {0, 0, static_cast<int>(ddW + 1.f), static_cast<int>(ddH + 1.f)};
    hasSubmenu = submenuOpen_ >= 0 && submenuOpen_ < static_cast<int>(submenus_.size());
    submenuRect = {0, 0, 0, 0};
    if (!hasSubmenu) return;
    auto& sm = submenus_[submenuOpen_];
    float sw = 180.f;
    for (auto& item : sm) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 36.f;
        if (w + 48.f > sw) sw = w + 48.f;
    }
    float sx = ddW + 2.f, sy = 2.f + submenuOpen_ * itemH;
    float maxH = windowH * 0.8f, sh = 2.f + sm.size() * itemH;
    if (sy + sh > windowH - 8.f) sy = windowH - sh - 8.f;
    if (sh > maxH) sh = maxH;
    if (windowW < 30000 && sx + sw > windowW - 4.f) sx = -sw - 2.f;
    submenuRect = {static_cast<int>(sx), static_cast<int>(sy), static_cast<int>(sw + 1.f), static_cast<int>(sh + 1.f)};
}

void Titlebar::drawMenu(FontAtlas& font) {
    if (!menuOpen_) return;
    if (menuDeferred_) { menuDeferred_ = false; return; }
    drawMenuPopup(font, 0.f, 0.f);
}

void Titlebar::drawMenuPopup(FontAtlas& font, float ox, float oy) {
    if (!menuOpen_) return;
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    int ww = 1280, wh = 720;
    if (window) SDL_GL_GetDrawableSize(window, &ww, &wh);
    float maxW = 140.f;
    for (auto& item : menuItems_) {
        float w = font.measureText(item.label);
        if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 24.f;
        if (w + 32.f > maxW) maxW = w + 32.f;
    }
    float ddX = -ox, ddY = height_ - oy;
    float itemH = 24.f;
    int visibleCount = (int)menuItems_.size();
    float arrowH = 0.f;
    float ddW = maxW, ddH = 2.f + visibleCount * itemH + arrowH * 2.f;
    lastMenuX_ = ddX + ox; lastMenuY_ = ddY + oy; lastMenuW_ = ddW; lastMenuH_ = ddH;
    lastHasSubmenu_ = false; lastSubmenuW_ = 0.f; lastSubmenuH_ = 0.f;
    std::vector<float> v;
    auto ar = [&](float x0,float y0,float x1,float y1,float r,float g,float b,float a) {
        v.insert(v.end(),{x0,y0,0,0,r,g,b,a, x0,y1,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x0,y0,0,0,r,g,b,a, x1,y1,0,0,r,g,b,a, x1,y0,0,0,r,g,b,a});
    };
    ar(ddX, ddY, ddX + ddW, ddY + ddH, 0.17f, 0.17f, 0.20f, 0.98f);
    ar(ddX, ddY, ddX + ddW, ddY + 1, 0.30f, 0.30f, 0.35f, 1.f);
    if (menuHovered_ >= menuScroll_ && menuHovered_ < menuScroll_ + visibleCount && !menuItems_[menuHovered_].separator)
        ar(ddX + 2, ddY + arrowH + 2 + (menuHovered_ - menuScroll_) * itemH, ddX + ddW - 2, ddY + arrowH + 2 + (menuHovered_ - menuScroll_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
    flushSolid(v);
    glEnable(GL_SCISSOR_TEST);
    glScissor((int)ddX, wh - (int)(ddY + ddH + 4), (int)ddW, (int)(ddH + 4));
    for (int i = menuScroll_; i < menuScroll_ + visibleCount && i < (int)menuItems_.size(); ++i) {
        auto& item = menuItems_[i];
        float rowY = ddY + arrowH + 2 + (i - menuScroll_) * itemH;
        float iy = rowY + 4.f;
        if (item.separator) {
            ar(ddX + 8, rowY + 11, ddX + ddW - 8, rowY + 12, 0.3f, 0.3f, 0.33f, 1.f);
            flushSolid(v);
            continue;
        }
        float ib = (menuHovered_ == i) ? 1.f : 0.78f;
        if (!item.action && !item.separator) ib = 0.4f;
        font.drawText(item.label, ddX + 12.f, iy, ib, ib, ib, 1.f);
        if (!item.shortcut.empty())
            font.drawText(item.shortcut, ddX + ddW - font.measureText(item.shortcut) - 12.f, iy, 0.5f, 0.5f, 0.55f, 1.f);
    }
    glDisable(GL_SCISSOR_TEST);
    if (submenuOpen_ >= 0 && submenuOpen_ < (int)submenus_.size()) {
        auto& sm = submenus_[submenuOpen_];
        float sx = ddX + ddW + 2.f, sy = ddY + 2.f + submenuOpen_ * itemH;
        float sw = 180.f;
        for (auto& item : sm) {
            float w = font.measureText(item.label);
            if (!item.shortcut.empty()) w += font.measureText(item.shortcut) + 36.f;
            if (w + 48.f > sw) sw = w + 48.f;
        }
        float sh = 2.f + static_cast<float>(sm.size()) * itemH;
        bool popupLocal = ox != 0.f || oy != 0.f;
        if (!popupLocal) {
            float maxH = wh * 0.8f;
            if (sy + sh > wh - 8.f) sy = wh - sh - 8.f;
            if (sh > maxH) sh = maxH;
            if (sx + sw > ww - 4.f) sx = ddX - sw - 2.f;
        }
        lastHasSubmenu_ = true; lastSubmenuX_ = sx + ox; lastSubmenuY_ = sy + oy; lastSubmenuW_ = sw; lastSubmenuH_ = sh;
        v.clear();
        ar(sx, sy, sx + sw, sy + sh, 0.17f, 0.17f, 0.20f, 0.98f);
        if (submenuHovered_ >= (int)sm.size() || (submenuHovered_ >= 0 && sm[submenuHovered_].separator))
            submenuHovered_ = -1;
        if (submenuHovered_ >= 0 && submenuHovered_ < (int)sm.size())
            ar(sx + 2, sy + 2 + submenuHovered_ * itemH, sx + sw - 2, sy + 2 + (submenuHovered_ + 1) * itemH, 0.25f, 0.30f, 0.45f, 1.f);
        flushSolid(v);
        glEnable(GL_SCISSOR_TEST);
        glScissor((int)sx, wh - (int)(sy + sh), (int)sw, (int)sh);
        for (int i = 0; i < (int)sm.size(); ++i) {
            auto& item = sm[i];
            float iy = sy + 6.f + i * itemH;
            if (item.separator) { v.clear(); ar(sx + 8, sy + 13.f + i * itemH, sx + sw - 8, sy + 14.f + i * itemH, 0.3f, 0.3f, 0.33f, 1.f); flushSolid(v); continue; }
            float b = (submenuHovered_ == i) ? 1.f : 0.78f;
            if (!item.action && !item.separator) b = 0.4f;
            font.drawText(item.label, sx + 12.f, iy, b, b, b, 1.f);
            if (!item.shortcut.empty()) font.drawText(item.shortcut, sx + sw - font.measureText(item.shortcut) - 12.f, iy, 0.5f, 0.5f, 0.55f, 1.f);
        }
        glDisable(GL_SCISSOR_TEST);
    }
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
    float titleY = 8.f;
    font.drawText(title_, titleX, titleY, 0.7f, 0.7f, 0.7f, 1.f);
    float mid = height_ / 2.f;
    font.drawText("_", buttons_[1].x + 16.f, mid - 4.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xe2\x96\xa1", buttons_[2].x + 16.f, mid - 8.f, 0.7f, 0.7f, 0.7f, 1.f);
    font.drawText("\xc3\x97", buttons_[3].x + 16.f, mid - 8.f, 1.f, 1.f, 1.f, 1.f);
    drawMenu(font);
}

bool Titlebar::handleMenuEvent(const SDL_Event& e) {
    if (!menuOpen_) return false;
    SDL_Window* window = SDL_GL_GetCurrentWindow();
    int ww = 1280, wh = 720;
    if (window) SDL_GL_GetDrawableSize(window, &ww, &wh);
    float ddX = lastMenuX_, ddY = lastMenuY_;
    float ddW = lastMenuW_, itemH = 24.f;
    int visibleCount = (int)menuItems_.size();
    float arrowH = 0.f;
    float ddH = lastMenuH_ > 0.f ? lastMenuH_ : 2.f + visibleCount * itemH + arrowH * 2.f;
    auto clampMenuScroll = [&] {
        int maxScroll = (int)menuItems_.size() - visibleCount;
        if (menuScroll_ < 0) menuScroll_ = 0;
        if (menuScroll_ > maxScroll) menuScroll_ = maxScroll;
    };
    if (e.type == SDL_MOUSEMOTION) {
        float mx = (float)e.motion.x, my = (float)e.motion.y;
        menuHovered_ = -1;
        if (mx >= ddX && mx < ddX + ddW && my >= ddY && my < ddY + ddH) {
            int idx = menuScroll_ + (int)((my - ddY - arrowH - 2) / itemH);
            if (idx >= 0 && idx < (int)menuItems_.size() && !menuItems_[idx].separator) { menuHovered_ = idx; submenuOpen_ = menuItems_[idx].submenu; submenuHovered_ = -1; }
        } else if (lastHasSubmenu_) {
            float sx = lastSubmenuX_, sy = lastSubmenuY_, sw = lastSubmenuW_;
            if (mx >= sx && mx < sx + sw && my >= sy + 2.f && my < sy + 2.f + submenus_[submenuOpen_].size() * itemH) {
                int idx = (int)((my - sy - 2.f) / itemH);
                submenuHovered_ = (idx >= 0 && submenuOpen_ >= 0 && submenuOpen_ < (int)submenus_.size() && idx < (int)submenus_[submenuOpen_].size() && !submenus_[submenuOpen_][idx].separator) ? idx : -1;
            }
            else submenuHovered_ = -1;
        }
        return true;
    }
    if (e.type == SDL_MOUSEWHEEL) {
        menuScroll_ -= e.wheel.y;
        clampMenuScroll();
        return true;
    }
    if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_ESCAPE) { closeMenu(); return true; }
        if (e.key.keysym.sym == SDLK_UP) { --menuScroll_; clampMenuScroll(); return true; }
        if (e.key.keysym.sym == SDLK_DOWN) { ++menuScroll_; clampMenuScroll(); return true; }
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        float mx = (float)e.button.x, my = (float)e.button.y;
        if (lastHasSubmenu_) {
            float sx = lastSubmenuX_, sy = lastSubmenuY_, sw = lastSubmenuW_;
            if (mx >= sx && mx < sx + sw && my >= sy + 2.f && my < sy + 2.f + submenus_[submenuOpen_].size() * itemH) {
                int idx = (int)((my - sy - 2.f) / itemH);
                auto& sm = submenus_[submenuOpen_];
                if (idx >= 0 && idx < (int)sm.size() && !sm[idx].separator && sm[idx].action) { sm[idx].action(); closeMenu(); submenuOpen_ = -1; return true; }
                return true;
            }
        }
        if (mx >= ddX && mx < ddX + ddW && my >= ddY && my < ddY + ddH) {
            int idx = menuScroll_ + (int)((my - ddY - arrowH - 2) / itemH);
            if (idx >= 0 && idx < (int)menuItems_.size() && !menuItems_[idx].separator && menuItems_[idx].action) {
                menuItems_[idx].action();
                closeMenu();
                return true;
            }
        }
        closeMenu(); submenuOpen_ = -1;
        return true;
    }
    return false;
}

SDL_HitTestResult Titlebar::hitTest(int mx, int my, SDL_Window* window) {
    int ww, wh; SDL_GetWindowSize(window, &ww, &wh);
    int re = static_cast<int>(resizeEdge_);
    bool left = mx >= 0 && mx < re;
    bool right = mx >= ww - re && mx < ww;
    bool top = my >= 0 && my < re;
    bool bottom = my >= wh - re && my < wh;
    if (left && top) return SDL_HITTEST_RESIZE_TOPLEFT;
    if (right && top) return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (left && bottom) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (right && bottom) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (top) return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    if (right) return SDL_HITTEST_RESIZE_RIGHT;
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
