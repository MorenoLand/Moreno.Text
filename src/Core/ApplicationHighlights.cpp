// Highlights drawn behind the text: Find matches, other occurrences of the selection, and the bracket pair at the cursor.
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Syntax/SyntaxHighlighter.h"
#include <algorithm>

#ifdef None
#undef None
#endif

size_t Application::matchingBracketOf(size_t at) const {
    if (at >= textBuffer.size()) return std::string::npos;
    char c = textBuffer[at];
    auto isOpen = [](char ch) { return ch == '(' || ch == '[' || ch == '{'; };
    auto isClose = [](char ch) { return ch == ')' || ch == ']' || ch == '}'; };
    if (!isOpen(c) && !isClose(c)) return std::string::npos;
    char pair = c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c == ')' ? '(' : c == ']' ? '[' : '{';
    const size_t limit = 40000;
    int depth = 0;
    if (isOpen(c)) {
        size_t end = std::min(textBuffer.size(), at + limit);
        for (size_t i = at; i < end; ++i) {
            if (textBuffer[i] == c) ++depth;
            else if (textBuffer[i] == pair && --depth == 0) return i;
        }
    } else {
        size_t begin = at > limit ? at - limit : 0;
        for (size_t i = at + 1; i-- > begin;) {
            if (textBuffer[i] == c) ++depth;
            else if (textBuffer[i] == pair && --depth == 0) return i;
        }
    }
    return std::string::npos;
}

void Application::drawEditorHighlights(float drawTextX, float textOriginY, float lineStep, float top, float bottom, size_t firstLine, size_t lastLine,
                                       float gutterRight, float editorRight) {
    if (wordWrap_ || binaryView_ || selections_.empty()) return;
    auto box = [&](size_t from, size_t to, float r, float g, float b, float a, bool underlineOnly) {
        if (to <= from || to > textBuffer.size()) return;
        size_t line = lineOfPos(from);
        if (line < firstLine || line > lastLine || isFolded(line)) return;
        size_t ls = lineStartForLine(line);
        if (from < ls) return;
        float y0 = textOriginY + line * lineStep - scrollY_;
        if (y0 + lineStep < top || y0 > bottom) return;
        float x0 = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, from - ls));
        float x1 = drawTextX + fontAtlas().measureText(std::string_view(textBuffer.data() + ls, std::min(to, lineEnd(ls)) - ls));
        x0 = std::max(x0, gutterRight); x1 = std::min(x1, editorRight);
        if (x1 <= x0) return;
        auto& v = solidVerts_;
        auto rect = [&](float ax, float ay, float bx, float by) { v.insert(v.end(), {ax,ay,0,0,r,g,b,a, ax,by,0,0,r,g,b,a, bx,by,0,0,r,g,b,a, ax,ay,0,0,r,g,b,a, bx,by,0,0,r,g,b,a, bx,ay,0,0,r,g,b,a}); };
        if (underlineOnly) { rect(x0, y0 + lineStep - 2.f, x1, y0 + lineStep); return; }
        rect(x0, y0 + 1.f, x1, y0 + lineStep - 1.f);
    };
    size_t visibleStart = lineStartForLine(firstLine);
    size_t visibleEnd = lastLine + 1 < totalLines() ? lineStartForLine(lastLine + 1) : textBuffer.size();

    // Find matches
    if (find_.active && !find_.query.empty() && !find_.matches.empty()) {
        auto it = std::lower_bound(find_.matches.begin(), find_.matches.end(), visibleStart);
        size_t drawn = 0;
        for (; it != find_.matches.end() && *it < visibleEnd && drawn < 400; ++it, ++drawn) {
            size_t len = find_.regex ? matchLengthAt(*it) : find_.query.size();
            bool current = static_cast<size_t>(it - find_.matches.begin()) == find_.currentMatch;
            box(*it, *it + len, 0.95f, 0.78f, 0.25f, current ? 0.45f : 0.26f, false);
        }
    }

    const SelRange& primary = selections_.front();
    // Other occurrences of the selected text
    if (selections_.size() == 1 && primary.hasSelection()) {
        size_t a = primary.min(), b = primary.max();
        if (b - a >= 2 && b - a <= 100 && textBuffer.find('\n', a) >= b) {
            std::string needle = textBuffer.substr(a, b - a);
            size_t pos = visibleStart, drawn = 0;
            while (drawn < 300 && (pos = textBuffer.find(needle, pos)) != std::string::npos && pos < visibleEnd) {
                if (pos != a) { box(pos, pos + needle.size(), accentColor_.r, accentColor_.g, accentColor_.b, 0.20f, false); ++drawn; }
                pos += needle.size();
            }
        }
    }

    // Matching bracket
    if (selections_.size() == 1 && !primary.hasSelection()) {
        size_t c = primary.cursor;
        size_t at = std::string::npos;
        if (c < textBuffer.size() && matchingBracketOf(c) != std::string::npos) at = c;
        else if (c > 0 && matchingBracketOf(c - 1) != std::string::npos) at = c - 1;
        if (at != std::string::npos) {
            size_t other = matchingBracketOf(at);
            box(at, at + 1, accentColor_.r, accentColor_.g, accentColor_.b, 0.35f, false);
            box(other, other + 1, accentColor_.r, accentColor_.g, accentColor_.b, 0.35f, false);
        }
    }
}
