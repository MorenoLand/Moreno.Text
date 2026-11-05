// The About and File Associations dialogs, drawn by the editor itself over the middle of the window.
#include "Core/Application.h"
#include "Core/Version.h"
#include "Renderer/FontAtlas.h"
#include "Renderer/GLRenderer.h"
#include "Theme/UiTheme.h"
#include "UI/LineEdit.h"
#include "UI/Titlebar.h"
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include "about_logo.h"

extern GLuint gl_vao();
extern GLuint gl_vbo();

namespace {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

enum AssocButton { BtnSelectAll, BtnClear, BtnAdd, BtnSettings, BtnApply, BtnClose, BtnTitleClose, BtnCount };

struct AssocLayout {
    Rect panel, list, addField, status;
    Rect buttons[BtnCount];
    float rowH = 22.f;
    int visibleRows = 0;
};

AssocLayout assocLayout(float ww, float wh, float top) {
    AssocLayout l;
    l.panel.w = std::min(580.f, ww - 40.f);
    l.panel.h = std::min(580.f, wh - top - 24.f);
    l.panel.x = std::floor((ww - l.panel.w) / 2.f);
    l.panel.y = std::max(top + 8.f, std::floor((wh - l.panel.h) / 2.f));
    const float px = l.panel.x, py = l.panel.y, pw = l.panel.w, ph = l.panel.h;
    l.buttons[BtnTitleClose] = {px + pw - 40.f, py, 40.f, 36.f};
    l.list = {px + 16.f, py + 72.f, pw - 32.f, std::max(80.f, ph - 72.f - 150.f)};
    l.visibleRows = std::max(1, static_cast<int>(l.list.h / l.rowH));
    l.list.h = l.visibleRows * l.rowH;
    float fy = l.list.y + l.list.h + 10.f;
    l.buttons[BtnSelectAll] = {px + 16.f, fy, 92.f, 26.f};
    l.buttons[BtnClear] = {px + 114.f, fy, 70.f, 26.f};
    l.buttons[BtnAdd] = {px + pw - 16.f - 64.f, fy, 64.f, 26.f};
    l.addField = {px + 330.f, fy, pw - 330.f - 16.f - 64.f - 6.f, 26.f};
    l.status = {px + 16.f, fy + 34.f, pw - 32.f, 50.f};
    float by = py + ph - 42.f;
    l.buttons[BtnSettings] = {px + 16.f, by, 190.f, 28.f};
    l.buttons[BtnClose] = {px + pw - 16.f - 80.f, by, 80.f, 28.f};
    l.buttons[BtnApply] = {px + pw - 16.f - 80.f - 8.f - 80.f, by, 80.f, 28.f};
    return l;
}

Rect aboutRect(float ww, float wh, float top) {
    Rect r;
    r.w = std::min(440.f, ww - 40.f);
    r.h = 268.f;
    r.x = std::floor((ww - r.w) / 2.f);
    r.y = std::max(top + 8.f, std::floor((wh - r.h) / 2.f));
    return r;
}

Rect aboutLink(const Rect& panel) { return {panel.x + 40.f, panel.y + 226.f, panel.w - 80.f, 22.f}; }
Rect aboutClose(const Rect& panel) { return {panel.x + panel.w - 40.f, panel.y, 40.f, 36.f}; }

// The large logo for the About dialog, uploaded on first use with mipmaps so it stays smooth when scaled down.
GLuint aboutLogoTexture() {
    static GLuint tex = 0;
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, about_logo_width, about_logo_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, about_logo_data);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    return tex;
}

// Shortens text with an ellipsis so it never runs past maxW.
std::string fitLine(FontAtlas& font, std::string text, float maxW) {
    if (font.measureText(text) <= maxW) return text;
    while (!text.empty() && font.measureText(text + "...") > maxW) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    }
    return text + "...";
}

std::vector<std::string> wrapText(FontAtlas& font, const std::string& text, float maxW) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flushWord = [&] {
        if (word.empty()) return;
        std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && font.measureText(trial) > maxW) { lines.push_back(line); line = word; }
        else line = trial;
        word.clear();
    };
    for (char c : text) { if (c == ' ') flushWord(); else word += c; }
    flushWord();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

}  // namespace

void Application::drawDialogs(float ww, float wh) {
    if (!aboutOpen_ && !assocOpen_) return;
    UiTheme::ChromeScope chrome;
    auto& ui = UiTheme::instance();
    const float top = titlebar_->height();
    FontAtlas& font = fontAtlas();
    std::vector<float> v;
    auto rect = [&](float x0, float y0, float x1, float y1, ThemeColor c, float a) {
        v.insert(v.end(), {x0,y0,0,0,c.r,c.g,c.b,a, x0,y1,0,0,c.r,c.g,c.b,a, x1,y1,0,0,c.r,c.g,c.b,a, x0,y0,0,0,c.r,c.g,c.b,a, x1,y1,0,0,c.r,c.g,c.b,a, x1,y0,0,0,c.r,c.g,c.b,a});
    };
    auto flush = [&] {
        if (v.empty()) return;
        GLRenderer::setDrawMode(2);
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size() / 8));
        glBindVertexArray(0); GLRenderer::setDrawMode(0);
        v.clear();
    };
    // Rounded corners the way the tabs do them: one-pixel rows that step in near the corner.
    auto roundRect = [&](float x0, float y0, float x1, float y1, float radius, ThemeColor c, float a, bool top = true, bool bottom = true) {
        radius = std::min(radius, std::min(x1 - x0, y1 - y0) * 0.5f);
        int rows = static_cast<int>(radius);
        for (int i = 0; i < rows; ++i) {
            float dy = radius - (static_cast<float>(i) + 0.5f);
            float inset = radius - std::sqrt(std::max(0.f, radius * radius - dy * dy));
            rect(top ? x0 + inset : x0, y0 + i, top ? x1 - inset : x1, y0 + i + 1.f, c, a);
            rect(bottom ? x0 + inset : x0, y1 - i - 1.f, bottom ? x1 - inset : x1, y1 - i, c, a);
        }
        rect(x0, y0 + rows, x1, y1 - rows, c, a);
    };
    auto frame = [&](const Rect& r, ThemeColor fill, ThemeColor edge, float radius = 5.f) {
        roundRect(r.x, r.y, r.x + r.w, r.y + r.h, radius, edge, 1.f);
        roundRect(r.x + 1.f, r.y + 1.f, r.x + r.w - 1.f, r.y + r.h - 1.f, std::max(0.f, radius - 1.f), fill, 1.f);
    };
    auto drawLogo = [&](float x, float y, float size, GLuint texture = 0) {
        if (!texture) texture = titlebar_->iconTexture();
        if (!texture) return;
        GLRenderer::setDrawMode(1);
        std::vector<float> iv = {x,y,0,0,1,1,1,1, x,y+size,0,1,1,1,1,1, x+size,y+size,1,1,1,1,1,1, x,y,0,0,1,1,1,1, x+size,y+size,1,1,1,1,1,1, x+size,y,1,0,1,1,1,1};
        glBindVertexArray(gl_vao()); glBindBuffer(GL_ARRAY_BUFFER, gl_vbo());
        glBufferData(GL_ARRAY_BUFFER, iv.size() * sizeof(float), iv.data(), GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_2D, texture); glDrawArrays(GL_TRIANGLES, 0, 6); glBindVertexArray(0);
        GLRenderer::setDrawMode(0);
    };
    const ThemeColor panelBg = ui.m(0.15f, 0.15f, 0.18f), panelEdge = ui.m(0.30f, 0.30f, 0.35f), titleBg = ui.m(0.11f, 0.11f, 0.13f);
    const ThemeColor fieldBg = ui.m(0.10f, 0.10f, 0.12f), button = ui.m(0.24f, 0.28f, 0.34f), buttonHot = ui.m(0.31f, 0.36f, 0.45f);
    const ThemeColor textColor = ui.m(0.86f, 0.86f, 0.89f), dim = ui.m(0.55f, 0.55f, 0.60f), accent = ui.m(0.34f, 0.56f, 0.90f);
    auto label = [&](const std::string& s, float x, float y, ThemeColor c) { font.drawText(s, x, y, c.r, c.g, c.b, 1.f); };
    auto centered = [&](const std::string& s, const Rect& r, ThemeColor c) {
        font.drawText(s, r.x + (r.w - font.measureText(s)) * 0.5f, r.y + (r.h - font.lineHeight()) * 0.5f, c.r, c.g, c.b, 1.f);
    };

    if (aboutOpen_) {
        Rect p = aboutRect(ww, wh, top);
        frame(p, panelBg, panelEdge, 9.f);
        roundRect(p.x + 1.f, p.y + 1.f, p.x + p.w - 1.f, p.y + 36.f, 8.f, titleBg, 1.f, true, false);
        Rect close = aboutClose(p);
        if (assocHoverButton_ == BtnTitleClose) roundRect(close.x, close.y + 1.f, close.x + close.w - 1.f, close.y + close.h, 8.f, ThemeColor{0.85f, 0.15f, 0.15f}, 1.f, true, false);
        flush();
        drawLogo(p.x + 12.f, p.y + 9.f, 18.f);
        label("About Moreno Text", p.x + 38.f, p.y + (36.f - font.lineHeight()) * 0.5f, textColor);
        centered("\xc3\x97", close, textColor);
        drawLogo(p.x + (p.w - 72.f) * 0.5f, p.y + 50.f, 72.f, aboutLogoTexture());
        auto middle = [&](const std::string& s, float y, ThemeColor c, float scale) {
            float w = font.measureText(s) * scale;
            font.drawTextScaled(s, p.x + (p.w - w) * 0.5f, y, scale, c.r, c.g, c.b, 1.f);
        };
        middle("Moreno Text", p.y + 130.f, accent, 1.6f);
        middle(std::string("Version ") + MORENO_TEXT_VERSION, p.y + 160.f, dim, 1.f);
        middle("A fast, scriptable, open-source text editor", p.y + 184.f, textColor, 1.f);
        middle("Licensed under the Mozilla Public License 2.0", p.y + 204.f, dim, 1.f);
        Rect link = aboutLink(p);
        std::string url = "github.com/MorenoLand/Moreno.Text";
        float uw = font.measureText(url), ux = p.x + (p.w - uw) * 0.5f, uy = p.y + 228.f;
        label(url, ux, uy, accent);
        rect(ux, uy + font.lineHeight() - 1.f, ux + uw, uy + font.lineHeight(), accent, 1.f);
        (void)link;
        flush();
        return;
    }

    AssocLayout l = assocLayout(ww, wh, top);
    frame(l.panel, panelBg, panelEdge, 9.f);
    roundRect(l.panel.x + 1.f, l.panel.y + 1.f, l.panel.x + l.panel.w - 1.f, l.panel.y + 36.f, 8.f, titleBg, 1.f, true, false);
    const Rect& tc = l.buttons[BtnTitleClose];
    if (assocHoverButton_ == BtnTitleClose) roundRect(tc.x, tc.y + 1.f, tc.x + tc.w - 1.f, tc.y + tc.h, 8.f, ThemeColor{0.85f, 0.15f, 0.15f}, 1.f, true, false);
    frame(l.list, fieldBg, panelEdge);
    const int total = static_cast<int>(assocEntries_.size());
    const int maxScroll = std::max(0, total - l.visibleRows);
    assocScroll_ = std::clamp(assocScroll_, 0, maxScroll);
    for (int i = 0; i < l.visibleRows && assocScroll_ + i < total; ++i) {
        float ry = l.list.y + i * l.rowH;
        if (assocScroll_ + i == assocHoverRow_) rect(l.list.x + 1.f, ry, l.list.x + l.list.w - 1.f, ry + l.rowH, ui.m(0.20f, 0.24f, 0.34f), 1.f);
        bool on = assocEntries_[assocScroll_ + i].checked;
        float bx = l.list.x + 10.f, by = ry + 4.f;
        rect(bx, by, bx + 14.f, by + 14.f, on ? accent : ui.m(0.50f, 0.50f, 0.56f), 1.f);
        rect(bx + 1.f, by + 1.f, bx + 13.f, by + 13.f, on ? accent : fieldBg, 1.f);
        if (on) rect(bx + 4.f, by + 4.f, bx + 10.f, by + 10.f, ui.m(0.95f, 0.95f, 0.98f), 1.f);
    }
    if (maxScroll > 0) {
        float track = l.list.h - 4.f, thumb = std::max(24.f, track * l.visibleRows / total);
        float ty = l.list.y + 2.f + (track - thumb) * assocScroll_ / maxScroll;
        rect(l.list.x + l.list.w - 8.f, ty, l.list.x + l.list.w - 3.f, ty + thumb, ui.m(0.37f, 0.37f, 0.40f), 1.f);
    }
    for (int b = 0; b < BtnTitleClose; ++b) {
        const Rect& r = l.buttons[b];
        roundRect(r.x, r.y, r.x + r.w, r.y + r.h, 4.f, b == assocHoverButton_ ? buttonHot : (b == BtnApply ? ui.m(0.22f, 0.36f, 0.58f) : button), 1.f);
    }
    frame(l.addField, fieldBg, panelEdge);
    {   // selection behind the text typed into the Add box
        std::string& t = assocAdd_;
        size_t c = std::min(assocAddCur_, t.size()), a = std::min(assocAddAnc_, t.size());
        float tx = l.addField.x + 6.f;
        if (c != a) {
            size_t s0 = std::min(c, a), s1 = std::max(c, a);
            float sx = tx + font.measureText(t.substr(0, s0)), sw = font.measureText(t.substr(s0, s1 - s0));
            rect(sx, l.addField.y + 4.f, sx + sw, l.addField.y + l.addField.h - 4.f, ThemeColor{0.22f, 0.36f, 0.65f}, 0.95f);
        }
        float cx = tx + font.measureText(t.substr(0, c));
        rect(cx, l.addField.y + 4.f, cx + 1.f, l.addField.y + l.addField.h - 4.f, textColor, 1.f);
    }
    flush();

    drawLogo(l.panel.x + 12.f, l.panel.y + 9.f, 18.f);
    label("File Associations", l.panel.x + 38.f, l.panel.y + (36.f - font.lineHeight()) * 0.5f, textColor);
    centered("\xc3\x97", tc, textColor);
    label(fitLine(font, "Tick the types Moreno Text should open. Untick to release them.", l.panel.w - 32.f), l.panel.x + 16.f, l.panel.y + 46.f, dim);
    for (int i = 0; i < l.visibleRows && assocScroll_ + i < total; ++i) {
        const auto& entry = assocEntries_[assocScroll_ + i];
        float ry = l.list.y + i * l.rowH + 3.f;
        label("." + entry.ext, l.list.x + 34.f, ry, textColor);
        bool mine = entry.opensWith == "Moreno Text";
        label(entry.opensWith, l.list.x + l.list.w * 0.46f, ry, mine ? accent : dim);
    }
    centered("Select All", l.buttons[BtnSelectAll], textColor);
    centered("Clear", l.buttons[BtnClear], textColor);
    centered("Add", l.buttons[BtnAdd], textColor);
    label("Add types:", l.addField.x - 8.f - font.measureText("Add types:"), l.addField.y + 4.f, dim);
    centered("Windows Default Apps...", l.buttons[BtnSettings], textColor);
    centered("Apply", l.buttons[BtnApply], textColor);
    centered("Close", l.buttons[BtnClose], textColor);
    {
        glEnable(GL_SCISSOR_TEST);
        GLRenderer::scissor(static_cast<int>(l.addField.x + 2.f), static_cast<int>(wh - (l.addField.y + l.addField.h)), static_cast<int>(l.addField.w - 4.f), static_cast<int>(l.addField.h));
        label(assocAdd_, l.addField.x + 6.f, l.addField.y + 4.f, textColor);
        glDisable(GL_SCISSOR_TEST);
    }
    float sy = l.status.y;
    for (const std::string& line : wrapText(font, assocStatus_, l.status.w)) {
        if (sy + font.lineHeight() > l.status.y + l.status.h) break;
        label(line, l.status.x, sy, dim);
        sy += font.lineHeight() + 2.f;
    }
}

bool Application::handleDialogEvent(const SDL_Event& e) {
    if (!aboutOpen_ && !assocOpen_) return false;
    const bool mouse = e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP || e.type == SDL_MOUSEWHEEL;
    const bool keyboard = e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT || e.type == SDL_TEXTEDITING;
    if (!mouse && !keyboard) return false;   // window and quit events still go through
    int iw = 0, ih = 0;
    SDL_GetWindowSize(window_, &iw, &ih);
    const float ww = static_cast<float>(iw), wh = static_cast<float>(ih), top = titlebar_->height();
    float mx = 0.f, my = 0.f;
    if (e.type == SDL_MOUSEMOTION) { mx = static_cast<float>(e.motion.x); my = static_cast<float>(e.motion.y); }
    else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) { mx = static_cast<float>(e.button.x); my = static_cast<float>(e.button.y); }
    else if (e.type == SDL_MOUSEWHEEL) { int x = 0, y = 0; SDL_GetMouseState(&x, &y); mx = static_cast<float>(x); my = static_cast<float>(y); }
    if (mouse && my < top) { assocHoverButton_ = -1; return false; }   // the title bar keeps working (move, minimise, close)

    if (aboutOpen_) {
        Rect p = aboutRect(ww, wh, top);
        if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE)) aboutOpen_ = false;
        if (e.type == SDL_MOUSEMOTION) assocHoverButton_ = aboutClose(p).contains(mx, my) ? BtnTitleClose : -1;
        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            if (aboutLink(p).contains(mx, my)) openDocumentation();
            aboutOpen_ = false;
            assocHoverButton_ = -1;
        }
        return true;
    }

    AssocLayout l = assocLayout(ww, wh, top);
    const int total = static_cast<int>(assocEntries_.size());
    const int maxScroll = std::max(0, total - l.visibleRows);
    auto buttonAt = [&](float x, float y) { for (int b = 0; b < BtnCount; ++b) if (l.buttons[b].contains(x, y)) return b; return -1; };
    auto rowAt = [&](float x, float y) { if (!l.list.contains(x, y)) return -1; int r = assocScroll_ + static_cast<int>((y - l.list.y) / l.rowH); return r < total ? r : -1; };
    auto addTypes = [&] {
        std::string text = assocAdd_, added;
        size_t start = 0;
        while (start <= text.size()) {
            size_t cut = text.find_first_of(", ;", start);
            std::string ext = FileAssociations::normalize(text.substr(start, cut == std::string::npos ? std::string::npos : cut - start));
            if (!ext.empty()) {
                auto it = std::find_if(assocEntries_.begin(), assocEntries_.end(), [&](const FileAssociations::Entry& x) { return x.ext == ext; });
                if (it == assocEntries_.end()) {
                    FileAssociations::Entry entry; entry.ext = ext;
                    assocEntries_.push_back(entry);
                    FileAssociations::refresh(assocEntries_);
                    it = assocEntries_.end() - 1;
                }
                it->checked = true;
                assocScroll_ = std::max(0, static_cast<int>(it - assocEntries_.begin()) - l.visibleRows / 2);
                added += (added.empty() ? "." : ", .") + ext;
            }
            if (cut == std::string::npos) break;
            start = cut + 1;
        }
        if (added.empty()) { assocStatus_ = "Type an extension such as gs2 or .toml, then press Add."; return; }
        assocAdd_.clear(); assocAddCur_ = assocAddAnc_ = 0;
        assocStatus_ = "Ticked " + added + ". Press Apply to register.";
    };

    if (e.type == SDL_MOUSEMOTION) {
        assocHoverButton_ = buttonAt(mx, my);
        assocHoverRow_ = assocHoverButton_ < 0 ? rowAt(mx, my) : -1;
    } else if (e.type == SDL_MOUSEWHEEL) {
        if (l.panel.contains(mx, my)) assocScroll_ = std::clamp(assocScroll_ - e.wheel.y * 3, 0, maxScroll);
    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int b = buttonAt(mx, my);
        if (b == BtnTitleClose || b == BtnClose) assocOpen_ = false;
        else if (b == BtnSelectAll) { for (auto& x : assocEntries_) x.checked = true; }
        else if (b == BtnClear) { for (auto& x : assocEntries_) x.checked = false; }
        else if (b == BtnAdd) addTypes();
        else if (b == BtnSettings) FileAssociations::openDefaultAppsSettings();
        else if (b == BtnApply) assocStatus_ = FileAssociations::apply(assocEntries_);
        else if (l.addField.contains(mx, my)) {
            float x0 = l.addField.x + 6.f;
            size_t pos = LineEdit::offsetAtX(assocAdd_, mx - x0, [&](const std::string& s) { return fontAtlas().measureText(s); });
            if (e.button.clicks >= 2) LineEdit::selectWordAt(assocAdd_, pos, assocAddCur_, assocAddAnc_);
            else { assocAddCur_ = pos; if (!(SDL_GetModState() & KMOD_SHIFT)) assocAddAnc_ = pos; }
        } else if (int r = rowAt(mx, my); r >= 0) assocEntries_[r].checked = !assocEntries_[r].checked;
        else if (!l.panel.contains(mx, my)) assocOpen_ = false;
    } else if (e.type == SDL_KEYDOWN) {
        SDL_Keycode sym = e.key.keysym.sym;
        if (sym == SDLK_ESCAPE) assocOpen_ = false;
        else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) { if (!assocAdd_.empty()) addTypes(); else assocStatus_ = FileAssociations::apply(assocEntries_); }
        else if (sym == SDLK_PAGEUP) assocScroll_ = std::clamp(assocScroll_ - l.visibleRows, 0, maxScroll);
        else if (sym == SDLK_PAGEDOWN) assocScroll_ = std::clamp(assocScroll_ + l.visibleRows, 0, maxScroll);
        else if (sym == SDLK_UP) assocScroll_ = std::clamp(assocScroll_ - 1, 0, maxScroll);
        else if (sym == SDLK_DOWN) assocScroll_ = std::clamp(assocScroll_ + 1, 0, maxScroll);
        else LineEdit::key(assocAdd_, assocAddCur_, assocAddAnc_, e.key);
    } else if (e.type == SDL_TEXTINPUT) {
        if (assocAdd_.size() < 80) LineEdit::insert(assocAdd_, assocAddCur_, assocAddAnc_, e.text.text);
    }
    return true;
}
