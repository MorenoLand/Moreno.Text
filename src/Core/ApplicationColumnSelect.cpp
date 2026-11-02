// Middle-button drag selects a rectangular block, one selection per line. A middle click without a drag still starts
// auto-scroll as before.
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "UI/Titlebar.h"
#include "UI/Gutter.h"
#include "UI/StatusBar.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>

#ifdef None
#undef None
#endif

namespace {
// Byte offset in [lineStart, lineEnd] whose left edge is nearest the pixel column x (relative to the text origin).
size_t offsetAtX(const std::string& text, size_t ls, size_t le, float x, FontAtlas& font) {
    size_t pos = ls;
    float prev = 0.f;
    while (pos < le) {
        size_t next = pos + 1;
        while (next < le && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
        float w = font.measureText(std::string_view(text.data() + ls, next - ls));
        if (x < (prev + w) * 0.5f) return pos;
        prev = w;
        pos = next;
    }
    return le;
}
}  // namespace

bool Application::handleMiddleColumnSelect(const SDL_Event& e) {
    if (binaryView_ || largeFileGuarded_ || wordWrap_ || editorGroups_.size() > 1) return false;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_, &ww, &wh);
    float tbH = titlebar_->height() + tabBarH_;
    float textX = (sidebarVisible_ ? sidebarWidth_ : 0.f) + gutter_->width() + 8.f;
    float lineStep = fontAtlas().lineHeight();
    float originY = tbH + fontAtlas().ascent() + 5.0f;
    auto lineAt = [&](float y) {
        float rel = (y - originY + scrollY_) / lineStep;
        long line = static_cast<long>(std::floor(rel));
        return static_cast<size_t>(std::clamp<long>(line, 0, static_cast<long>(totalLines()) - 1));
    };
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == 2) {
        if (autoScrollActive_) { autoScrollActive_ = false; return true; }
        float x = static_cast<float>(e.button.x), y = static_cast<float>(e.button.y);
        if (x < textX - 8.f || y < tbH || y >= static_cast<float>(wh) - statusbar_->height()) return false;
        columnDragging_ = true;
        columnMoved_ = false;
        columnStartX_ = x; columnStartY_ = y;
        columnStartLine_ = lineAt(y);
        return true;
    }
    if (!columnDragging_) return false;
    if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_MMASK)) {
        float x = static_cast<float>(e.motion.x), y = static_cast<float>(e.motion.y);
        if (!columnMoved_ && std::abs(x - columnStartX_) + std::abs(y - columnStartY_) < 4.f) return true;
        columnMoved_ = true;
        size_t a = columnStartLine_, b = lineAt(y);
        if (a > b) std::swap(a, b);
        float x0 = columnStartX_ - textX + scrollX_, x1 = x - textX + scrollX_;
        std::vector<SelRange> block;
        for (size_t line = a; line <= b; ++line) {
            size_t ls = lineStartForLine(line), le = lineEnd(ls);
            block.emplace_back(offsetAtX(textBuffer, ls, le, x0, fontAtlas()), offsetAtX(textBuffer, ls, le, x1, fontAtlas()));
        }
        if (!block.empty()) selections_ = std::move(block);
        return true;
    }
    if (e.type == SDL_MOUSEBUTTONUP && e.button.button == 2) {
        columnDragging_ = false;
        if (!columnMoved_) {  // a plain middle click: auto-scroll, like before
            autoScrollActive_ = true;
            autoScrollAnchorX_ = columnStartX_;
            autoScrollAnchorY_ = columnStartY_;
            autoScrollLastTick_ = SDL_GetTicks();
        }
        return true;
    }
    return false;
}
