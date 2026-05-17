#include "Platform/Platform.h"
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shobjidl.h>
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
    bool pickFolder(std::string& outPath) {
        outPath.clear();
        HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        bool shouldUninit = SUCCEEDED(init);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;

        IFileDialog* dialog = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr) && dialog) {
            DWORD options = 0;
            if (SUCCEEDED(dialog->GetOptions(&options))) {
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            }
            if (SUCCEEDED(dialog->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                    PWSTR widePath = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &widePath)) && widePath) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 1) {
                            outPath.resize((size_t)len - 1);
                            WideCharToMultiByte(CP_UTF8, 0, widePath, -1, outPath.data(), len, nullptr, nullptr);
                        }
                        CoTaskMemFree(widePath);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (shouldUninit) CoUninitialize();
        return !outPath.empty();
    }
}
#else
#include <iostream>
namespace Platform {
    void setRoundedCorners(void*, int) {}
    bool isWindows11OrLater() { return false; }
    bool pickFolder(std::string& outPath) {
        std::cout << "Open folder path: ";
        std::getline(std::cin, outPath);
        return !outPath.empty();
    }
}
#endif
