#include "Platform/Platform.h"
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

namespace Platform {
    void setRoundedCorners(void* nativeHandle, int radius) {
        HWND hwnd = static_cast<HWND>(nativeHandle);
        // DWMWA_WINDOW_CORNER_PREFERENCE: Win11 22H2+
        // 0=Default, 1=DoNotRound, 2=Round, 3=RoundSmall
        enum DWM_WINDOW_CORNER_PREFERENCE { DWMWCP_DEFAULT = 0, DWMWCP_DONOTROUND = 1, DWMWCP_ROUND = 2, DWMWCP_ROUNDSMALL = 3 };
        DWM_WINDOW_CORNER_PREFERENCE pref = radius > 4 ? DWMWCP_ROUND : DWMWCP_ROUNDSMALL;
        HRESULT hr = DwmSetWindowAttribute(hwnd, 33, &pref, sizeof(pref));
        (void)hr;
    }
    bool isWindows11OrLater() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        auto RtlGetVersion = reinterpret_cast<LONG(WINAPI*)(PRTL_OSVERSIONINFOW)>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (!RtlGetVersion) return false;
        RTL_OSVERSIONINFOW vi = { sizeof(vi) };
        RtlGetVersion(&vi);
        return vi.dwBuildNumber >= 22000;
    }
}
#else
namespace Platform {
    void setRoundedCorners(void*, int) {}
    bool isWindows11OrLater() { return false; }
}
#endif
