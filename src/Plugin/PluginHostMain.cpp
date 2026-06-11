#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static fs::path exeDir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
#else
    return fs::current_path();
#endif
}

static std::string envValue(const char* name) {
#ifdef _WIN32
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) return {};
    std::string value(needed, '\0');
    DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    value.resize(written);
    return value;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

static void appendLog(const std::string& message) {
    std::string logPath = envValue("MORENO_CONSOLE_LOG");
    if (logPath.empty()) return;
    try {
        fs::create_directories(fs::path(logPath).parent_path());
        std::ofstream log(logPath, std::ios::binary | std::ios::app);
        log << message << "\n";
    } catch (...) {
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        appendLog("[plugin-host] missing bootstrap path");
        return 2;
    }

    fs::path root = exeDir();
    fs::path runtime = root / "Data" / "Lib" / "Runtime" / "Python38";
    fs::path pythonDll = runtime / "python38.dll";
    fs::path pythonZip = runtime / "python38.zip";
    if (!fs::exists(pythonDll) || !fs::exists(pythonZip)) {
        appendLog("[plugin-host] bundled Python runtime incomplete: " + runtime.string());
        return 3;
    }

    SetDllDirectoryW(runtime.wstring().c_str());
    HMODULE python = LoadLibraryW(pythonDll.wstring().c_str());
    if (!python) {
        appendLog("[plugin-host] failed to load python38.dll: " + std::to_string(GetLastError()));
        return 4;
    }

    using PyMainFn = int(__cdecl*)(int, wchar_t**);
    using PySetWideStringFn = void(__cdecl*)(const wchar_t*);
    auto pyMain = reinterpret_cast<PyMainFn>(GetProcAddress(python, "Py_Main"));
    auto pySetPythonHome = reinterpret_cast<PySetWideStringFn>(GetProcAddress(python, "Py_SetPythonHome"));
    auto pySetProgramName = reinterpret_cast<PySetWideStringFn>(GetProcAddress(python, "Py_SetProgramName"));
    if (!pyMain || !pySetPythonHome || !pySetProgramName) {
        appendLog("[plugin-host] python38.dll missing required embedding exports");
        return 5;
    }

    std::wstring home = runtime.wstring();
    std::wstring program = (root / "moreno_plugin_host.exe").wstring();
    pySetPythonHome(home.c_str());
    pySetProgramName(program.c_str());

    std::vector<std::wstring> argStorage;
    argStorage.reserve((size_t)argc);
    argStorage.push_back(program);
    for (int i = 1; i < argc; ++i) argStorage.push_back(argv[i]);

    std::vector<wchar_t*> pyArgv;
    pyArgv.reserve(argStorage.size());
    for (auto& arg : argStorage) pyArgv.push_back(arg.data());

    return pyMain((int)pyArgv.size(), pyArgv.data());
}
#else
int main() {
    appendLog("[plugin-host] bundled plugin host is currently implemented for Windows");
    return 6;
}
#endif
