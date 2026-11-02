// Requests the plugin host makes of the editor and waits on: message boxes, file dialogs and the clipboard.
#include "Core/Application.h"
#include "Platform/Platform.h"
#include <SDL2/SDL.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
void writeResponse(const std::string& path, const json& body) {
    if (path.empty()) return;
    try {
        std::string tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << body.dump();
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
    } catch (...) {}
}
}  // namespace

void Application::handlePluginRpc(const std::string& type, const json& j) {
    std::string response = j.value("response", "");
    if (type == "clipboard_set") {
        SDL_SetClipboardText(j.value("text", "").c_str());
        return;
    }
    if (type == "clipboard_get") {
        char* text = SDL_GetClipboardText();
        writeResponse(response, {{"text", text ? text : ""}});
        if (text) SDL_free(text);
        return;
    }
    if (type == "dialog") {
        std::vector<std::string> labels = j.value("buttons", std::vector<std::string>{"OK"});
        if (labels.empty()) labels.push_back("OK");
        std::vector<SDL_MessageBoxButtonData> buttons;
        for (size_t i = 0; i < labels.size(); ++i) {
            SDL_MessageBoxButtonData b = {};
            b.flags = i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0;
            if (i + 1 == labels.size() && labels.size() > 1) b.flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
            b.buttonid = static_cast<int>(i);
            b.text = labels[i].c_str();
            buttons.push_back(b);
        }
        std::string message = j.value("message", "");
        SDL_MessageBoxData data = {};
        data.flags = SDL_MESSAGEBOX_INFORMATION;
        data.window = window_;
        data.title = "Moreno Text";
        data.message = message.c_str();
        data.numbuttons = static_cast<int>(buttons.size());
        data.buttons = buttons.data();
        int chosen = -1;
        inOsDialog_ = true;
        SDL_ShowMessageBox(&data, &chosen);
        inOsDialog_ = false;
        writeResponse(response, {{"button", chosen}});
        return;
    }
    if (type == "file_dialog") {
        std::string mode = j.value("mode", "open");
        std::string path;
        inOsDialog_ = true;
        if (mode == "folder") {
            Platform::pickFolder(path);
        } else {
#ifdef _WIN32
            char buf[MAX_PATH] = {};
            std::string name = j.value("name", "");
            std::snprintf(buf, sizeof(buf), "%s", name.c_str());
            std::string dir = j.value("directory", "");
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.lpstrFilter = "All Files\0*.*\0";
            ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
            if (!dir.empty()) ofn.lpstrInitialDir = dir.c_str();
            bool picked;
            if (mode == "save") { ofn.Flags = OFN_OVERWRITEPROMPT; picked = GetSaveFileNameA(&ofn) != 0; }
            else { ofn.Flags = OFN_FILEMUSTEXIST; picked = GetOpenFileNameA(&ofn) != 0; }
            if (picked) path = buf;
#endif
        }
        inOsDialog_ = false;
        json paths = json::array();
        if (!path.empty()) paths.push_back(path);
        writeResponse(response, {{"paths", paths}});
    }
}
