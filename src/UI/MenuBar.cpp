#include "UI/MenuBar.h"
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>

extern GLuint gl_vao();
extern GLuint gl_vbo();

void MenuBar::init() {
    // File
    Menu file; file.title = "File";
    file.items = {
        {"New", "Ctrl+N", []{ Application::instance().newBuffer(); }},
        {"Open...", "Ctrl+O", []{ /* handled by keybinding */ }},
        {"Save", "Ctrl+S", []{ Application::instance().saveFile(); }},
        {"Save As...", "Ctrl+Shift+S", []{ Application::instance().saveFileAs(); }},
        {"", "", nullptr, true},
        {"Close", "Ctrl+W", []{ Application::instance().newBuffer(); }},
        {"", "", nullptr, true},
        {"Exit", "Ctrl+Q", []{ Application::instance().quit(); }},
    };
    menus_.push_back(std::move(file));
    // Edit
    Menu edit; edit.title = "Edit";
    edit.items = {
        {"Undo", "Ctrl+Z", []{}}, {"Redo", "Ctrl+Y", []{}},
        {"", "", nullptr, true},
        {"Cut", "Ctrl+X", []{}}, {"Copy", "Ctrl+C", []{}}, {"Paste", "Ctrl+V", []{}},
        {"Select All", "Ctrl+A", []{}},
    };
    menus_.push_back(std::move(edit));
    // Selection
    Menu sel; sel.title = "Selection";
    sel.items = {
        {"Add Next Line", "Ctrl+L", []{}},
        {"Split into Lines", "Ctrl+Shift+L", []{}},
        {"Find Next Occurrence", "Ctrl+D", []{}},
    };
    menus_.push_back(std::move(sel));
    // Find
    Menu find; find.title = "Find";
    find.items = {
        {"Find...", "Ctrl+F", []{}},
        {"Replace...", "Ctrl+H", []{}},
    };
    menus_.push_back(std::move(find));
    // View
    Menu view; view.title = "View";
    view.items = {
        {"Goto Anything...", "Ctrl+P", []{}},
    };
    menus_.push_back(std::move(view));
    // Goto
    Menu gto; gto.title = "Goto";
    gto.items = {{"Goto Line...", "Ctrl+G", []{}}, {"Goto Definition", "F12", []{}}};
    menus_.push_back(std::move(gto));
    // Tools
    Menu tools; tools.title = "Tools";
    tools.items = {{"Build", "Ctrl+B", []{}}, {"Build With...", "Ctrl+Shift+B", []{}}};
    menus_.push_back(std::move(tools));
    // Project
    Menu proj; proj.title = "Project";
    proj.items = {{"Open Project...", "", []{}}, {"Save Project", "", []{}}, {"Close Project", "", []{}}};
    menus_.push_back(std::move(proj));
    // Preferences
    Menu prefs; prefs.title = "Preferences";
    prefs.items = {{"Settings", "Ctrl+,", []{}}, {"Key Bindings", "", []{}}, {"Color Scheme", "", []{}}, {"Font", "", []{}}};
    menus_.push_back(std::move(prefs));
    // Help
    Menu help; help.title = "Help";
    help.items = {{"About Moreno Text", "", []{}}};
    menus_.push_back(std::move(help));
}

void MenuBar::closeAll() {
    for (auto& m : menus_) m.open = false;
    anyOpen_ = false;
    hoveredMenu_ = -1;
    hoveredItem_ = -1;
}

void MenuBar::draw(FontAtlas& font, float windowW, float titlebarH) {
    std::vector<float> verts;
    auto addRect = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
        verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a, x0,y1, 0,0, r,g,b,a, x1,y1, 0,0, r,g,b,a });
        verts.insert(verts.end(), { x0,y0, 0,0, r,g,b,a, x1,y1, 0,0, r,g,b,a, x1,y0, 0,0, r,g,b,a });
    };
    float barY = titlebarH;
    // bar background
    addRect(0, barY, windowW, barY + height_, 0.14f, 0.14f, 0.17f, 1.f);
    addRect(0, barY + height_, windowW, barY + height_ + 1, 0.10f, 0.10f, 0.12f, 1.f);
    // menu titles
    float x = 38.f; // after icon
    for (int mi = 0; mi < (int)menus_.size(); ++mi) {
        auto& m = menus_[mi];
        float tw = font.measureText(m.title) + 16.f;
        m.x = x; m.w = tw;
        if (m.open || hoveredMenu_ == mi)
            addRect(x, barY, x + tw, barY + height_, m.open ? 0.22f : 0.18f, m.open ? 0.22f : 0.18f, m.open ? 0.26f : 0.21f, 1.f);
        float brightness = (hoveredMenu_ == mi && anyOpen_) ? 1.f : 0.72f;
        font.drawText(m.title, x + 8.f, barY + 4.f, brightness, brightness, brightness, 1.f);
        x += tw;
    }
    // flush bar bg + highlights
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
    // dropdown for open menu
    for (int mi = 0; mi < (int)menus_.size(); ++mi) {
        if (!menus_[mi].open) continue;
        auto& m = menus_[mi];
        float ddW = 220.f;
        float ddY = barY + height_;
        verts.clear();
        // shadow + bg
        addRect(m.x, ddY, m.x + ddW, ddY + 4 + m.items.size() * 24.f, 0.16f, 0.16f, 0.19f, 1.f);
        addRect(m.x, ddY, m.x + ddW, ddY + 2 + m.items.size() * 24.f, 0.18f, 0.18f, 0.21f, 1.f);
        // hover highlight
        if (hoveredItem_ >= 0 && hoveredItem_ < (int)m.items.size()) {
            auto& item = m.items[hoveredItem_];
            if (!item.separator)
                addRect(m.x + 2, ddY + 2 + hoveredItem_ * 24.f, m.x + ddW - 2, ddY + 2 + (hoveredItem_ + 1) * 24.f, 0.25f, 0.30f, 0.45f, 1.f);
        }
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao());
        glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 8));
        glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
        // item text
        for (int ii = 0; ii < (int)m.items.size(); ++ii) {
            auto& item = m.items[ii];
            if (item.separator) {
                // draw separator line
                verts.clear();
                addRect(m.x + 8, ddY + 2 + ii * 24.f + 11, m.x + ddW - 8, ddY + 2 + ii * 24.f + 12, 0.3f, 0.3f, 0.33f, 1.f);
                GLRenderer::setDrawMode(2);
                glBindVertexArray(gl_vao());
                glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
                glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, 0);
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(verts.size() / 8));
                glBindVertexArray(0);
                GLRenderer::setDrawMode(0);
                continue;
            }
            float iy = ddY + 2 + ii * 24.f + 4.f;
            float ib = (hoveredItem_ == ii) ? 1.f : 0.78f;
            font.drawText(item.label, m.x + 12.f, iy, ib, ib, ib, 1.f);
            if (!item.shortcut.empty())
                font.drawText(item.shortcut, m.x + ddW - font.measureText(item.shortcut) - 12.f, iy, 0.5f, 0.5f, 0.55f, 1.f);
        }
    }
}

bool MenuBar::handleEvent(const SDL_Event& e, float windowW, float titlebarH) {
    float barY = titlebarH;
    if (e.type == SDL_MOUSEMOTION) {
        float mx = (float)e.motion.x, my = (float)e.motion.y;
        hoveredItem_ = -1;
        hoveredMenu_ = -1;
        if (my >= barY && my < barY + height_) {
            float x = 38.f;
            for (int mi = 0; mi < (int)menus_.size(); ++mi) {
                if (mx >= menus_[mi].x && mx < menus_[mi].x + menus_[mi].w) { hoveredMenu_ = mi; break; }
                x += menus_[mi].w;
            }
            if (anyOpen_ && hoveredMenu_ >= 0) {
                for (auto& m : menus_) m.open = false;
                menus_[hoveredMenu_].open = true;
            }
        } else if (anyOpen_) {
            // check dropdowns
            for (int mi = 0; mi < (int)menus_.size(); ++mi) {
                if (!menus_[mi].open) continue;
                auto& m = menus_[mi];
                float ddW = 220.f, ddY = barY + height_;
                if (mx >= m.x && mx <= m.x + ddW && my >= ddY) {
                    hoveredMenu_ = mi;
                    int idx = (int)((my - ddY - 2) / 24.f);
                    if (idx >= 0 && idx < (int)m.items.size()) hoveredItem_ = idx;
                }
            }
        }
        return anyOpen_;
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 1) {
        float mx = (float)e.button.x, my = (float)e.button.y;
        if (my >= barY && my < barY + height_) {
            for (int mi = 0; mi < (int)menus_.size(); ++mi) {
                if (mx >= menus_[mi].x && mx < menus_[mi].x + menus_[mi].w) {
                    if (menus_[mi].open) closeAll();
                    else { closeAll(); menus_[mi].open = true; anyOpen_ = true; }
                    return true;
                }
            }
            closeAll();
            return false;
        }
        // check dropdown click
        if (anyOpen_) {
            for (int mi = 0; mi < (int)menus_.size(); ++mi) {
                if (!menus_[mi].open) continue;
                auto& m = menus_[mi];
                float ddW = 220.f, ddY = barY + height_;
                if (mx >= m.x && mx <= m.x + ddW && my >= ddY) {
                    int idx = (int)((my - ddY - 2) / 24.f);
                    if (idx >= 0 && idx < (int)m.items.size() && !m.items[idx].separator && m.items[idx].action) {
                        m.items[idx].action();
                        closeAll();
                        return true;
                    }
                }
            }
            closeAll();
            return true;
        }
    }
    return false;
}
