// Pre-warmed spare window. Starting a window costs most of a second (graphics driver, fonts), so one hidden spare
// process is kept ready; dragging a tab out hands the tab to it instead of starting a new process.
//
// Files in <Local>/DetachedTabs, where <pid> is the spare's process id:
//   warm-<pid>.starting   written by whoever spawned the spare
//   warm-<pid>.ready      the spare finished starting up and is waiting
//   warm-<pid>.claimed    a window took it (rename is atomic, so only one window can)
//   warm-<pid>.go.json    what the claimed spare should show: {"handoff", "x", "y"}
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include <SDL2/SDL.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

fs::path warmDir(const std::string& localDir) { return fs::path(localDir) / "DetachedTabs"; }

// "warm-1234.ready" -> 1234, or 0.
unsigned long warmPidOf(const fs::path& file) {
    std::string name = file.filename().string();
    if (name.rfind("warm-", 0) != 0) return 0;
    size_t dot = name.find('.');
    if (dot == std::string::npos || dot <= 5) return 0;
    try { return std::stoul(name.substr(5, dot - 5)); } catch (...) { return 0; }
}

bool processAlive(unsigned long pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    (void)pid;
    return false;
#endif
}

}  // namespace

// Starts a spare if none is running or starting.
void Application::ensureWarmSpare() {
#ifdef _WIN32
    if (warmIdle_ || !SettingsManager::instance().get<bool>("prewarm_detached_window", true)) return;
    fs::path dir = warmDir(paths_.localDir);
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string ext = it->path().extension().string();
        if (ext != ".starting" && ext != ".ready") continue;
        unsigned long pid = warmPidOf(it->path());
        if (pid && processAlive(pid)) return;  // one is already on its way or ready
        fs::remove(it->path(), ec);
    }
    fs::path exe = fs::path(paths_.exeDir) / "moreno_text.exe";
    if (!fs::exists(exe, ec)) return;
    std::string command = "\"" + exe.string() + "\" --warm " + std::to_string(GetCurrentProcessId());
    STARTUPINFOA si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd(command.begin(), command.end());
    cmd.push_back('\0');
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, BELOW_NORMAL_PRIORITY_CLASS, nullptr, paths_.exeDir.c_str(), &si, &pi)) return;
    std::ofstream(dir / ("warm-" + std::to_string(pi.dwProcessId) + ".starting")) << "starting";
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#endif
}

// Hands a detached tab to a ready spare. Returns false when there is none, so the caller starts a new process.
bool Application::claimWarmSpare(const std::string& handoff, int x, int y) {
#ifdef _WIN32
    if (warmIdle_ || !SettingsManager::instance().get<bool>("prewarm_detached_window", true)) return false;
    fs::path dir = warmDir(paths_.localDir);
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".ready") continue;
        unsigned long pid = warmPidOf(it->path());
        if (!pid || !processAlive(pid)) { fs::remove(it->path(), ec); continue; }
        fs::path claimed = it->path();
        claimed.replace_extension(".claimed");
        std::error_code renameEc;
        fs::rename(it->path(), claimed, renameEc);
        if (renameEc) continue;  // another window got it first
        fs::path go = dir / ("warm-" + std::to_string(pid) + ".go.json");
        fs::path tmp = go;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) { fs::remove(claimed, ec); continue; }
            out << nlohmann::json{{"handoff", handoff}, {"x", x}, {"y", y}}.dump();
        }
        fs::rename(tmp, go, ec);
        warmNextSpareCheck_ = SDL_GetTicks() + 500;  // start the next spare soon
        return true;
    }
#else
    (void)handoff; (void)x; (void)y;
#endif
    return false;
}

// Runs in the idle spare: leave when the window that started it is gone, and wake up when a tab arrives.
void Application::pollWarmSpare() {
#ifdef _WIN32
    uint32_t now = SDL_GetTicks();
    if (now - warmLastPoll_ < 100) return;
    warmLastPoll_ = now;
    if (warmOwnerPid_ && now - warmLastOwnerCheck_ > 1500) {
        warmLastOwnerCheck_ = now;
        if (!processAlive(warmOwnerPid_)) { running_ = false; return; }
    }
    fs::path go = warmDir(paths_.localDir) / ("warm-" + std::to_string(GetCurrentProcessId()) + ".go.json");
    std::error_code ec;
    if (!fs::exists(go, ec)) return;
    nlohmann::json request;
    try {
        std::ifstream in(go, std::ios::binary);
        request = nlohmann::json::parse(in, nullptr, false);
    } catch (...) {}
    fs::remove(go, ec);
    fs::remove(warmDir(paths_.localDir) / ("warm-" + std::to_string(GetCurrentProcessId()) + ".claimed"), ec);
    if (!request.is_object()) { running_ = false; return; }
    activateWarmSpare(request.value("handoff", std::string()), request.value("x", 0), request.value("y", 0));
#endif
}

void Application::removeWarmFiles() {
#ifdef _WIN32
    std::error_code ec;
    std::string base = "warm-" + std::to_string(GetCurrentProcessId());
    for (const char* ext : {".starting", ".ready", ".claimed", ".go.json"}) fs::remove(warmDir(paths_.localDir) / (base + ext), ec);
#endif
}

// The spare tells waiting windows it is ready by renaming its .starting file.
void Application::markWarmReady() {
#ifdef _WIN32
    std::error_code ec;
    fs::path base = warmDir(paths_.localDir) / ("warm-" + std::to_string(GetCurrentProcessId()));
    fs::path starting = base, ready = base;
    starting += ".starting";
    ready += ".ready";
    fs::rename(starting, ready, ec);
    if (ec) std::ofstream(ready) << "ready";  // the spawner hadn't written .starting yet
#endif
}
