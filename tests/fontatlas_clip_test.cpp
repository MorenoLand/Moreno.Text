#include "Renderer/FontAtlas.h"

int main() {
    if (FontAtlas::visibleGlyphReserve(1000000, 0.f, 900.f, 9.f) >= 120) return 1;
    if (FontAtlas::visibleGlyphReserve(12, 0.f, 900.f, 9.f) != 12) return 2;
    if (FontAtlas::visibleGlyphReserve(100, 500.f, 100.f, 9.f) != 0) return 3;
    AtlasGlyph glyph{};
    glyph.colored = true;
    if (!glyph.colored) return 4;
    return 0;
}
