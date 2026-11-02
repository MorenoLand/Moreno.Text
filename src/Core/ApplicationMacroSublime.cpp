// Sublime .sublime-macro files: a JSON array of {"command", "args"}. Recorded keystrokes are exported as commands,
// and macro files are played back by turning commands into the key events they stand for.
#include "Core/Application.h"
#include "Commands/KeyBindingManager.h"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <cstring>
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

SDL_Event keyEvent(SDL_Keycode sym, Uint16 mod = 0) {
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_KEYDOWN;
    ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = sym;
    ev.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    ev.key.keysym.mod = mod;
    return ev;
}

SDL_Event textEvent(const std::string& text) {
    SDL_Event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = SDL_TEXTINPUT;
    std::strncpy(ev.text.text, text.c_str(), sizeof(ev.text.text) - 1);
    return ev;
}

// The keys Sublime's own commands stand for; returns false when a command has no key equivalent.
bool commandToKeys(const std::string& command, const json& args, std::vector<SDL_Event>& out) {
    auto flag = [&](const char* name, bool def) { return args.is_object() && args.contains(name) && args[name].is_boolean() ? args[name].get<bool>() : def; };
    auto str = [&](const char* name) { return args.is_object() && args.contains(name) && args[name].is_string() ? args[name].get<std::string>() : std::string(); };
    Uint16 shift = flag("extend", false) ? KMOD_LSHIFT : 0;
    if (command == "left_delete") { out.push_back(keyEvent(SDLK_BACKSPACE)); return true; }
    if (command == "right_delete") { out.push_back(keyEvent(SDLK_DELETE)); return true; }
    if (command == "delete_word") { out.push_back(keyEvent(flag("forward", false) ? SDLK_DELETE : SDLK_BACKSPACE, KMOD_LCTRL)); return true; }
    if (command == "insert") {
        std::string chars = str("characters");
        for (size_t i = 0; i < chars.size();) {
            size_t len = 1;
            while (i + len < chars.size() && (static_cast<unsigned char>(chars[i + len]) & 0xC0) == 0x80) ++len;
            std::string ch = chars.substr(i, len);
            if (ch == "\n") out.push_back(keyEvent(SDLK_RETURN));
            else if (ch == "\t") out.push_back(keyEvent(SDLK_TAB));
            else out.push_back(textEvent(ch));
            i += len;
        }
        return true;
    }
    if (command == "move") {
        std::string by = str("by");
        bool forward = flag("forward", true);
        if (by == "characters") out.push_back(keyEvent(forward ? SDLK_RIGHT : SDLK_LEFT, shift));
        else if (by == "lines") out.push_back(keyEvent(forward ? SDLK_DOWN : SDLK_UP, shift));
        else if (by == "words" || by == "word_ends" || by == "subwords" || by == "subword_ends") out.push_back(keyEvent(forward ? SDLK_RIGHT : SDLK_LEFT, shift | KMOD_LCTRL));
        else if (by == "pages") out.push_back(keyEvent(forward ? SDLK_PAGEDOWN : SDLK_PAGEUP, shift));
        else return false;
        return true;
    }
    if (command == "move_to") {
        std::string to = str("to");
        if (to == "bol") out.push_back(keyEvent(SDLK_HOME, shift));
        else if (to == "eol") out.push_back(keyEvent(SDLK_END, shift));
        else if (to == "bof") out.push_back(keyEvent(SDLK_HOME, shift | KMOD_LCTRL));
        else if (to == "eof") out.push_back(keyEvent(SDLK_END, shift | KMOD_LCTRL));
        else return false;
        return true;
    }
    return false;
}

std::string comboFor(const SDL_KeyboardEvent& k) {
    auto sym = k.keysym.sym; auto mod = k.keysym.mod;
    bool ctrl = mod & KMOD_CTRL, shift = mod & KMOD_SHIFT;
    std::string key;
    if (ctrl) key += "ctrl+";
    if (mod & KMOD_ALT) key += "alt+";
    if (shift) key += "shift+";
    if (sym >= SDLK_a && sym <= SDLK_z) key += static_cast<char>(sym);
    else if (sym >= SDLK_0 && sym <= SDLK_9) key += static_cast<char>(sym);
    else if (sym >= SDLK_F1 && sym <= SDLK_F12) key += "f" + std::to_string(sym - SDLK_F1 + 1);
    else if (sym == SDLK_SLASH) key += "/";
    else if (sym == SDLK_BACKSLASH) key += "\\";
    else if (sym == SDLK_SPACE) key += "space";
    else if (sym == SDLK_RETURN) key += "enter";
    else if (sym == SDLK_TAB) key += "tab";
    else if (sym == SDLK_BACKSPACE) key += "backspace";
    else if (sym == SDLK_DELETE) key += "delete";
    else if (sym == SDLK_UP) key += "up";
    else if (sym == SDLK_DOWN) key += "down";
    else if (sym == SDLK_LEFT) key += "left";
    else if (sym == SDLK_RIGHT) key += "right";
    else if (sym == SDLK_HOME) key += "home";
    else if (sym == SDLK_END) key += "end";
    else if (sym == SDLK_PAGEUP) key += "pageup";
    else if (sym == SDLK_PAGEDOWN) key += "pagedown";
    else return {};
    return key;
}

}  // namespace

// The recorded keystrokes as Sublime commands. Keys with a binding become that command; typed text becomes insert.
json Application::macroAsSublime() const {
    json out = json::array();
    auto& kb = KeyBindingManager::instance();
    for (const SDL_Event& ev : macro_) {
        if (ev.type == SDL_TEXTINPUT) {
            out.push_back({{"args", {{"characters", std::string(ev.text.text)}}}, {"command", "insert"}});
            continue;
        }
        if (ev.type != SDL_KEYDOWN) continue;
        auto sym = ev.key.keysym.sym; auto mod = ev.key.keysym.mod;
        bool plain = !(mod & (KMOD_CTRL | KMOD_ALT));
        if (plain && ((sym >= SDLK_a && sym <= SDLK_z) || (sym >= SDLK_0 && sym <= SDLK_9) || sym == SDLK_SPACE)) continue;  // the text event carries it
        std::string combo = comboFor(ev.key);
        if (combo.empty()) continue;
        if (const KeyBinding* b = kb.find(combo)) { out.push_back({{"args", b->args.is_object() ? b->args : json::object()}, {"command", b->command}}); continue; }
        bool extend = mod & KMOD_SHIFT, ctrl = mod & KMOD_CTRL;
        auto move = [&](const char* by, bool forward) { out.push_back({{"args", {{"by", by}, {"forward", forward}, {"extend", extend}}}, {"command", "move"}}); };
        auto moveTo = [&](const char* to) { out.push_back({{"args", {{"to", to}, {"extend", extend}}}, {"command", "move_to"}}); };
        if (sym == SDLK_RETURN) out.push_back({{"args", {{"characters", "\n"}}}, {"command", "insert"}});
        else if (sym == SDLK_TAB) out.push_back({{"args", {{"characters", "\t"}}}, {"command", "insert"}});
        else if (sym == SDLK_BACKSPACE) out.push_back({{"command", ctrl ? "delete_word" : "left_delete"}, {"args", ctrl ? json{{"forward", false}} : json::object()}});
        else if (sym == SDLK_DELETE) out.push_back({{"command", ctrl ? "delete_word" : "right_delete"}, {"args", ctrl ? json{{"forward", true}} : json::object()}});
        else if (sym == SDLK_LEFT) move(ctrl ? "words" : "characters", false);
        else if (sym == SDLK_RIGHT) move(ctrl ? "words" : "characters", true);
        else if (sym == SDLK_UP) move("lines", false);
        else if (sym == SDLK_DOWN) move("lines", true);
        else if (sym == SDLK_PAGEUP) move("pages", false);
        else if (sym == SDLK_PAGEDOWN) move("pages", true);
        else if (sym == SDLK_HOME) moveTo(ctrl ? "bof" : "bol");
        else if (sym == SDLK_END) moveTo(ctrl ? "eof" : "eol");
    }
    return out;
}

void Application::saveMacroAs() {
    if (macro_.empty()) loadMacro();
    if (macro_.empty()) { pluginStatusText_ = "No macro recorded"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
#ifdef _WIN32
    closeAllPopups();
    fs::path dir = fs::path(paths_.dataDir) / "Packages" / "User";
    std::error_code ec;
    fs::create_directories(dir, ec);
    char buf[MAX_PATH] = {};
    std::snprintf(buf, sizeof(buf), "%s", (dir / "Macro.sublime-macro").string().c_str());
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Sublime Macro\0*.sublime-macro\0All Files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "sublime-macro";
    inOsDialog_ = true;
    bool picked = GetSaveFileNameA(&ofn) != 0;
    inOsDialog_ = false;
    if (!picked) return;
    std::ofstream out(buf, std::ios::binary | std::ios::trunc);
    out << macroAsSublime().dump(4);
    pluginStatusText_ = "Saved " + fs::path(buf).filename().string();
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
#endif
}

// Turns a parsed .sublime-macro into queued events; commands without a key equivalent run through the command table.
void Application::queueSublimeMacro(const json& commands) {
    if (!commands.is_array()) return;
    macroPlaying_ = true;
    for (const json& entry : commands) {
        if (!entry.is_object() || !entry.contains("command") || !entry["command"].is_string()) continue;
        std::string command = entry["command"].get<std::string>();
        json args = entry.contains("args") && entry["args"].is_object() ? entry["args"] : json::object();
        std::vector<SDL_Event> keys;
        if (commandToKeys(command, args, keys)) {
            for (SDL_Event& ev : keys) SDL_PushEvent(&ev);
        } else {
            macroCommandQueue_.emplace_back(command, args.dump());
            SDL_Event ev;
            std::memset(&ev, 0, sizeof(ev));
            ev.type = SDL_USEREVENT;
            ev.user.code = 0x4d43;
            SDL_PushEvent(&ev);
        }
    }
    SDL_Event done;
    std::memset(&done, 0, sizeof(done));
    done.type = SDL_USEREVENT;
    done.user.code = 0x4d41;
    SDL_PushEvent(&done);
}

void Application::runQueuedMacroCommand() {
    if (macroCommandQueue_.empty()) return;
    auto item = std::move(macroCommandQueue_.front());
    macroCommandQueue_.pop_front();
    KeyBindingManager::instance().runCommand(item.first, json::parse(item.second, nullptr, false));
}

void Application::runMacroFile(const std::string& file) {
    if (macroRecording_ || macroPlaying_) return;
    std::string path = file;
    if (path.empty()) {
#ifdef _WIN32
        closeAllPopups();
        char buf[MAX_PATH] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "Sublime Macro\0*.sublime-macro\0All Files\0*.*\0";
        ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
        inOsDialog_ = true;
        bool picked = GetOpenFileNameA(&ofn) != 0;
        inOsDialog_ = false;
        if (!picked) return;
        path = buf;
#else
        return;
#endif
    } else if (path.rfind("Packages/", 0) == 0) {
        path = (fs::path(paths_.packagesDir) / path.substr(9)).string();
    }
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) { pluginStatusText_ = "Macro not found: " + path; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto commands = json::parse(text, nullptr, false, true);
        if (!commands.is_array()) { pluginStatusText_ = "Not a macro file"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
        queueSublimeMacro(commands);
    } catch (...) {}
}
