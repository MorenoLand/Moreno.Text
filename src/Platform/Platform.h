#pragma once
#include <string>

namespace Platform {
    void setRoundedCorners(void* nativeHandle, int radius = 8);
    bool isWindows11OrLater();
}
