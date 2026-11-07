// Highlights drawn behind the text: Find matches, other occurrences of the selection, and the bracket pair at the cursor.
#include "Core/Application.h"
#include "Renderer/FontAtlas.h"
#include "Syntax/SyntaxHighlighter.h"
#include <algorithm>
#include <cctype>

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

namespace {

struct TagAt { size_t nameStart = 0, nameEnd = 0, tagEnd = 0; bool closing = false, selfClosing = false, valid = false; };

bool tagNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ':' || c == '.'; }

// Reads the tag whose '<' sits at `lt`.
TagAt readTag(const std::string& t, size_t lt) {
    TagAt tag;
    size_t i = lt + 1;
    if (i < t.size() && t[i] == '/') { tag.closing = true; ++i; }
    tag.nameStart = i;
    while (i < t.size() && tagNameChar(t[i])) ++i;
    tag.nameEnd = i;
    if (tag.nameEnd == tag.nameStart || !std::isalpha(static_cast<unsigned char>(t[tag.nameStart]))) return tag;
    char quote = 0;
    for (size_t n = 0; i < t.size() && n < 4000; ++i, ++n) {
        char c = t[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '<') return tag;
        else if (c == '>') { tag.tagEnd = i + 1; tag.selfClosing = i > 0 && t[i - 1] == '/'; tag.valid = true; return tag; }
    }
    return tag;
}

bool voidElement(const std::string& name) {
    static const char* names[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"};
    std::string low;
    for (char c : name) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto n : names) if (low == n) return true;
    return false;
}

// Finds the tag under `cursor` and its partner. Returns false when the cursor is not in a tag name or there is no partner.
bool findTagPair(const std::string& t, size_t cursor, TagAt& here, TagAt& other) {
    const size_t limit = 40000;
    size_t lt = std::string::npos;
    for (size_t i = std::min(cursor, t.size()), n = 0; i > 0 && n < 300; --i, ++n) {
        char c = t[i - 1];
        if (c == '>' || c == '\n') return false;
        if (c == '<') { lt = i - 1; break; }
    }
    if (lt == std::string::npos) return false;
    here = readTag(t, lt);
    if (!here.valid || here.selfClosing || cursor < here.nameStart || cursor > here.nameEnd) return false;
    std::string name = t.substr(here.nameStart, here.nameEnd - here.nameStart);
    if (!here.closing && voidElement(name)) return false;
    int depth = 0;
    if (!here.closing) {
        size_t end = std::min(t.size(), here.tagEnd + limit);
        for (size_t i = here.tagEnd; i < end; ++i) {
            if (t[i] != '<') continue;
            if (t.compare(i, 4, "<!--") == 0) { size_t e = t.find("-->", i + 4); if (e == std::string::npos) return false; i = e + 2; continue; }
            TagAt tag = readTag(t, i);
            if (!tag.valid || tag.selfClosing || t.compare(tag.nameStart, tag.nameEnd - tag.nameStart, name) != 0 || tag.nameEnd - tag.nameStart != name.size()) continue;
            if (!tag.closing) ++depth;
            else if (depth-- == 0) { other = tag; return true; }
        }
    } else {
        size_t begin = here.nameStart > limit ? here.nameStart - limit : 0;
        for (size_t i = here.nameStart - 1; i-- > begin;) {
            if (t[i] != '<') continue;
            TagAt tag = readTag(t, i);
            if (!tag.valid || tag.selfClosing || tag.nameEnd - tag.nameStart != name.size() || t.compare(tag.nameStart, name.size(), name) != 0) continue;
            if (tag.closing) ++depth;
            else if (depth-- == 0) { other = tag; return true; }
        }
    }
    return false;
}

}  // namespace

void Application::drawEditorHighlights(float drawTextX, float textOriginY, float lineStep, float top, float bottom, size_t firstLine, size_t lastLine,
                                       float gutterRight, float editorRight) {
    if (wordWrap_ || binaryView_ || selections_.empty()) return;
    auto box = [&](size_t from, size_t to, float r, float g, float b, float a, int mode) {
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
        if (mode == 1) { rect(x0, y0 + lineStep - 2.f, x1, y0 + lineStep); return; }
        if (mode == 2) { rect(x0, y0 + 1.f, x1, y0 + 2.f); rect(x0, y0 + lineStep - 2.f, x1, y0 + lineStep - 1.f); rect(x0, y0 + 1.f, x0 + 1.f, y0 + lineStep - 1.f); rect(x1 - 1.f, y0 + 1.f, x1, y0 + lineStep - 1.f); return; }
        rect(x0, y0 + 1.f, x1, y0 + lineStep - 1.f);
    };
    size_t visibleStart = lineStartForLine(firstLine);
    size_t visibleEnd = lastLine + 1 < totalLines() ? lineStartForLine(lastLine + 1) : textBuffer.size();

    // Regions a plugin added with view.add_regions
    if (!pluginRegions_.empty()) {
        auto found = pluginRegions_.find(pluginEventKey(activeTab_));
        if (found != pluginRegions_.end()) {
            for (auto& [name, set] : found->second) {
                const int kNoFill = 32, kHidden = 128, kNoOutline = 256, kUnderline = 512 | 1024 | 2048;
                if (set.flags & kHidden) continue;
                bool fill = !(set.flags & kNoFill), outline = !(set.flags & kNoOutline), underline = (set.flags & kUnderline) != 0;
                for (auto& [from, to] : set.ranges) {
                    if (to > textBuffer.size() || from >= to || to < visibleStart || from > visibleEnd) continue;
                    size_t line = lineOfPos(from), endLine = lineOfPos(to);
                    for (size_t l = std::max(line, firstLine); l <= endLine && l <= lastLine; ++l) {
                        size_t ls = lineStartForLine(l);
                        size_t a = std::max(from, ls), b = std::min(to, lineEnd(ls));
                        if (fill && !underline) box(a, b, set.r, set.g, set.b, 0.22f, 0);
                        if (underline) box(a, b, set.r, set.g, set.b, 0.95f, 1);
                        else if (outline) box(a, b, set.r, set.g, set.b, 0.7f, 2);
                    }
                }
            }
        }
    }

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
    if (matchSelection_ && selections_.size() == 1 && primary.hasSelection()) {
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

    // Matching HTML/XML tag
    if (matchTags_ && selections_.size() == 1 && !primary.hasSelection()) {
        TagAt here, other;
        if (findTagPair(textBuffer, primary.cursor, here, other)) {
            box(here.nameStart, here.nameEnd, accentColor_.r, accentColor_.g, accentColor_.b, 0.9f, 1);
            box(other.nameStart, other.nameEnd, accentColor_.r, accentColor_.g, accentColor_.b, 0.9f, 1);
        }
    }

    // Matching bracket
    if (matchBrackets_ && selections_.size() == 1 && !primary.hasSelection()) {
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
