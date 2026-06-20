#pragma once
#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <string>

namespace GLRenderer {
    bool init(int logicalWidth, int logicalHeight, int drawableWidth, int drawableHeight);
    void destroy();
    void resize(int logicalWidth, int logicalHeight, int drawableWidth, int drawableHeight);
    void beginFrame();
    void endFrame();
    void setDrawMode(int mode); // 0=font atlas, 1=rgba texture, 2=solid
    void scissor(float x, float y, float width, float height);
}
