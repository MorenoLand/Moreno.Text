// Keyboard macro recording and playback.
// A macro is the sequence of key and text events typed while recording; playback feeds them back
// through the normal event queue, so every command behaves exactly as it does when typed.
#include "Core/Application.h"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

std::string Application::macroPath() const {
    return (fs::path(paths_.dataDir) / "Packages" / "User" / "Recorded.moreno-macro").string();
}

void Application::toggleMacroRecording() {
    if (macroPlaying_) return;
    if (!macroRecording_) {
        macro_.clear();
        macroRecording_ = true;
        pluginStatusText_ = "Recording macro... (Ctrl+Q to stop)";
        pluginStatusUntil_ = SDL_GetTicks() + 600000;
    } else {
        macroRecording_ = false;
        pluginStatusText_ = "Macro recorded: " + std::to_string(macro_.size()) + " events (Ctrl+Shift+Q to play)";
        pluginStatusUntil_ = SDL_GetTicks() + 5000;
        saveMacro();
    }
}

void Application::playMacro() {
    if (macroRecording_ || macroPlaying_) return;
    if (macro_.empty()) loadMacro();
    if (macro_.empty()) {
        pluginStatusText_ = "No macro recorded";
        pluginStatusUntil_ = SDL_GetTicks() + 3000;
        return;
    }
    macroPlaying_ = true;
    for (SDL_Event ev : macro_) SDL_PushEvent(&ev);
    SDL_Event done;
    std::memset(&done, 0, sizeof(done));
    done.type = SDL_USEREVENT;
    done.user.code = 0x4d41;
    SDL_PushEvent(&done);
}

void Application::saveMacro() {
    if (macro_.empty()) return;
    try {
        nlohmann::json events = nlohmann::json::array();
        for (const SDL_Event& ev : macro_) {
            if (ev.type == SDL_KEYDOWN) events.push_back({{"type", "key"}, {"sym", static_cast<int>(ev.key.keysym.sym)}, {"scancode", static_cast<int>(ev.key.keysym.scancode)}, {"mod", static_cast<int>(ev.key.keysym.mod)}});
            else if (ev.type == SDL_TEXTINPUT) events.push_back({{"type", "text"}, {"text", std::string(ev.text.text)}});
        }
        fs::path path = macroPath();
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << nlohmann::json{{"events", events}}.dump(1);
    } catch (...) {}
}

void Application::loadMacro() {
    try {
        std::ifstream in(macroPath(), std::ios::binary);
        if (!in) return;
        nlohmann::json doc = nlohmann::json::parse(in);
        macro_.clear();
        for (auto& item : doc.value("events", nlohmann::json::array())) {
            SDL_Event ev;
            std::memset(&ev, 0, sizeof(ev));
            if (item.value("type", "") == "key") {
                ev.type = SDL_KEYDOWN;
                ev.key.state = SDL_PRESSED;
                ev.key.keysym.sym = item.value("sym", 0);
                ev.key.keysym.scancode = static_cast<SDL_Scancode>(item.value("scancode", 0));
                ev.key.keysym.mod = static_cast<Uint16>(item.value("mod", 0));
            } else if (item.value("type", "") == "text") {
                ev.type = SDL_TEXTINPUT;
                std::string text = item.value("text", "");
                std::strncpy(ev.text.text, text.c_str(), sizeof(ev.text.text) - 1);
            } else {
                continue;
            }
            macro_.push_back(ev);
        }
    } catch (...) {}
}
