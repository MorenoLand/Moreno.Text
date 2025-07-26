#pragma once
#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <string>

namespace GLRenderer {
    bool init(int width, int height);
    void destroy();
    void resize(int width, int height);
    void beginFrame();
    void endFrame();
    void setDrawMode(int mode); // 0=font atlas, 1=rgba texture, 2=solid
}
