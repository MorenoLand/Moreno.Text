#pragma once
#include <string>

class FontAtlas;

FontAtlas& fontAtlas();

namespace AppGlobals {
    extern std::string textBuffer;
}
