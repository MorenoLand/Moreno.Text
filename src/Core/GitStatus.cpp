#include "Core/GitStatus.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

// Runs `git <args>` in `dir`, returns what it wrote to stdout. Empty when git is missing or fails.
std::string runGit(const std::string& dir, const std::string& args) {
    std::string output;
#ifdef _WIN32
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return output;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writeEnd;
    si.hStdError = nullptr;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string command = "git -C \"" + dir + "\" " + args;
    if (CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(writeEnd);
        char buffer[8192];
        DWORD got = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got) output.append(buffer, got);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        CloseHandle(writeEnd);
    }
    CloseHandle(readEnd);
#else
    std::string command = "git -C \"" + dir + "\" " + args + " 2>/dev/null";
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[8192];
        size_t got;
        while ((got = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, got);
        pclose(pipe);
    }
#endif
    return output;
}

char classify(char index, char work) {
    if (index == 'U' || work == 'U' || (index == 'A' && work == 'A') || (index == 'D' && work == 'D')) return 'U';
    if (index == '?' && work == '?') return '?';
    if (index == 'A') return 'A';
    if (index == 'D' || work == 'D') return 'D';
    if (index == 'R' || index == 'C') return 'R';
    return 'M';
}

}  // namespace

std::string GitStatus::normalize(const std::string& path) {
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') c = '/';
#ifdef _WIN32
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

void GitStatus::parse(const std::string& root, const std::string& porcelain, Map& out) {
    const std::string base = normalize(root);
    size_t pos = 0;
    while (pos < porcelain.size()) {
        size_t end = porcelain.find('\0', pos);
        if (end == std::string::npos) end = porcelain.size();
        std::string entry = porcelain.substr(pos, end - pos);
        pos = end + 1;
        if (entry.size() < 4) continue;
        char index = entry[0], work = entry[1];
        std::string rel = entry.substr(3);
        if (index == 'R' || index == 'C') {   // a rename lists the old name next; the new one is what is on disk
            size_t oldEnd = porcelain.find('\0', pos);
            pos = oldEnd == std::string::npos ? porcelain.size() : oldEnd + 1;
        }
        if (index == '!' && work == '!') continue;
        char state = classify(index, work);
        std::string key = normalize(base + "/" + rel);
        out[key] = state;
        // folders that hold a change are marked too, all the way up to the repository root
        for (size_t slash = key.rfind('/'); slash != std::string::npos && slash >= base.size(); slash = key.rfind('/', slash - 1)) {
            std::string parent = key.substr(0, slash);
            auto it = out.find(parent);
            if (it == out.end()) out[parent] = 'M';
            if (slash == 0) break;
        }
    }
}

void GitStatus::collect(const std::string& dir, Map& out) {
    std::string top = runGit(dir, "rev-parse --show-toplevel");
    while (!top.empty() && (top.back() == '\n' || top.back() == '\r' || top.back() == ' ')) top.pop_back();
    if (top.empty()) return;
    parse(top, runGit(top, "status --porcelain=v1 -z"), out);
}
