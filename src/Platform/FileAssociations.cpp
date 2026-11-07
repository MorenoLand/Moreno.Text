#include "Platform/FileAssociations.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <set>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace FileAssociations {
namespace {

const wchar_t* const kProgId = L"MorenoText.File";
const wchar_t* const kAppKey = L"Applications\\moreno_text.exe";
const wchar_t* const kCapabilities = L"Software\\MorenoText\\Capabilities";   // what makes the app appear under Settings > Default apps

// ── registry helpers: always an explicit key path, never a pattern ──────────────────────────────────────
std::wstring classKey(const std::wstring& relative) { return L"Software\\Classes\\" + relative; }

bool readString(HKEY root, const std::wstring& sub, const wchar_t* name, std::wstring& out) {
    out.clear();
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool ok = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
    if (ok) {
        std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
        ok = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &size) == ERROR_SUCCESS;
        if (ok) out.assign(buffer.c_str());
    }
    RegCloseKey(key);
    return ok;
}

bool writeString(const std::wstring& sub, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool deleteValue(const std::wstring& sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    bool ok = RegDeleteValueW(key, name) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool hasValue(const std::wstring& sub, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    bool ok = RegQueryValueExW(key, name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

std::vector<std::wstring> valueNames(const std::wstring& sub) {
    std::vector<std::wstring> names;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return names;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]; DWORD len = 256;
        if (RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        names.emplace_back(name, len);
    }
    RegCloseKey(key);
    return names;
}

std::wstring exePath() {
    wchar_t path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    return std::wstring(path, n);
}

// "gs2", ".GS2" -> "gs2"; empty when the text cannot be a file extension.
std::wstring normalizeExtension(std::wstring text) {
    size_t a = text.find_first_not_of(L" \t.");
    size_t b = text.find_last_not_of(L" \t");
    if (a == std::wstring::npos) return {};
    text = text.substr(a, b - a + 1);
    if (text.empty() || text.size() > 32) return {};
    for (wchar_t& c : text) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-' || c == L'+';
        if (!ok) return {};
    }
    return text;
}

// ── what Windows currently does with an extension ───────────────────────────────────────────────────
struct State {
    bool registered = false;     // listed under "Open with"
    bool isDefault = false;      // Moreno Text is what opens it
    bool windowsChoice = false;  // the user's own Windows choice is in force (only Windows Settings can change it)
    std::wstring handler;        // the ProgId that opens it
    std::wstring description;    // shown in the list
};

std::wstring programName(const std::wstring& progId) {
    if (progId.rfind(L"Applications\\", 0) == 0) return progId.substr(13);
    std::wstring command;
    if (readString(HKEY_CLASSES_ROOT, progId + L"\\shell\\open\\command", nullptr, command) && !command.empty()) {
        std::wstring exe;
        if (command[0] == L'"') { size_t q = command.find(L'"', 1); exe = command.substr(1, q == std::wstring::npos ? std::wstring::npos : q - 1); }
        else exe = command.substr(0, command.find(L' '));
        size_t slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) exe = exe.substr(slash + 1);
        if (!exe.empty() && exe[0] == L'%') return L"(runs the file itself)";
        if (!exe.empty()) return exe;
    }
    return progId;
}

State inspect(const std::wstring& ext) {
    State s;
    std::wstring dot = L"." + ext;
    s.registered = hasValue(classKey(dot + L"\\OpenWithProgids"), kProgId);
    std::wstring userChoice;
    readString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + dot + L"\\UserChoice", L"ProgId", userChoice);
    std::wstring classDefault;
    if (!readString(HKEY_CURRENT_USER, classKey(dot), nullptr, classDefault) || classDefault.empty()) readString(HKEY_CLASSES_ROOT, dot, nullptr, classDefault);
    s.handler = userChoice.empty() ? classDefault : userChoice;
    s.windowsChoice = !userChoice.empty();
    s.isDefault = s.handler == kProgId || s.handler == kAppKey;
    if (s.isDefault) s.description = L"Moreno Text";
    else if (s.handler.empty()) s.description = L"(nothing set)";
    else s.description = programName(s.handler);
    return s;
}

// ── registering and unregistering ───────────────────────────────────────────────────────────────────
void registerProgId() {
    std::wstring exe = exePath();
    std::wstring open = L"\"" + exe + L"\" \"%1\"";
    std::wstring icon = L"\"" + exe + L"\",0";
    writeString(classKey(kProgId), nullptr, L"Moreno Text Document");
    writeString(classKey(std::wstring(kProgId) + L"\\DefaultIcon"), nullptr, icon);
    writeString(classKey(std::wstring(kProgId) + L"\\shell\\open\\command"), nullptr, open);
    std::wstring app = classKey(kAppKey);
    writeString(app, L"FriendlyAppName", L"Moreno Text");
    writeString(app + L"\\DefaultIcon", nullptr, icon);
    writeString(app + L"\\shell\\open\\command", nullptr, open);
    // Registered application: lists Moreno Text with its file types under Settings > Default apps, where Windows offers "Set default"
    writeString(kCapabilities, L"ApplicationName", L"Moreno Text");
    writeString(kCapabilities, L"ApplicationDescription", L"A fast, simple, open source text editor");
    writeString(L"Software\\RegisteredApplications", L"Moreno Text", kCapabilities);
}

// Returns a note for the user when Windows will not let us make Moreno Text the default (empty when all is well).
std::wstring applyExtension(const std::wstring& ext, bool wanted) {
    std::wstring dot = L"." + ext;
    std::wstring supported = classKey(std::wstring(kAppKey) + L"\\SupportedTypes");
    std::wstring previous = classKey(std::wstring(kProgId) + L"\\PreviousDefaults");
    std::wstring extKey = classKey(dot);
    if (wanted) {
        writeString(classKey(dot + L"\\OpenWithProgids"), kProgId, L"");
        writeString(supported, dot.c_str(), L"");
        writeString(std::wstring(kCapabilities) + L"\\FileAssociations", dot.c_str(), kProgId);
        State s = inspect(ext);
        if (s.windowsChoice && !s.isDefault) return L"." + ext + L" is set to " + s.description + L" in Windows; choose Moreno Text under Default apps.";
        std::wstring current;
        readString(HKEY_CURRENT_USER, extKey, nullptr, current);
        if (current != kProgId) {
            if (!current.empty() && !hasValue(previous, ext.c_str())) writeString(previous, ext.c_str(), current);   // remember it so unticking restores it
            writeString(extKey, nullptr, kProgId);
        }
        return {};
    }
    deleteValue(classKey(dot + L"\\OpenWithProgids"), kProgId);
    deleteValue(supported, dot.c_str());
    deleteValue(std::wstring(kCapabilities) + L"\\FileAssociations", dot.c_str());
    std::wstring current;
    if (readString(HKEY_CURRENT_USER, extKey, nullptr, current) && current == kProgId) {
        std::wstring before;
        if (readString(HKEY_CURRENT_USER, previous, ext.c_str(), before) && !before.empty()) { writeString(extKey, nullptr, before); deleteValue(previous, ext.c_str()); }
        else deleteValue(extKey, nullptr);
    }
    State s = inspect(ext);
    if (s.windowsChoice && s.isDefault) return L"." + ext + L" is still Moreno Text in Windows' Default apps; change it there.";
    return {};
}

void notifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr); }

const char* const kDefaultExtensions[] = {
    "txt", "text", "log", "md", "markdown", "ini", "cfg", "conf", "config", "cnf", "properties", "env", "json", "jsonc", "yml", "yaml", "toml", "xml",
    "csv", "tsv", "sh", "bash", "zsh", "bat", "cmd", "ps1", "psm1", "py", "pyw", "js", "mjs", "cjs", "ts", "tsx", "jsx", "css", "scss", "sass", "less",
    "html", "htm", "c", "h", "cc", "cpp", "cxx", "hpp", "hxx", "cs", "java", "kt", "go", "rs", "rb", "php", "pl", "lua", "sql", "gradle", "cmake",
    "mk", "diff", "patch", "gitignore", "gitattributes", "editorconfig", "sublime-settings", "sublime-keymap", "sublime-commands", "sublime-menu",
    "sublime-syntax", "sublime-color-scheme", "sublime-theme", "tmlanguage", "tmtheme", "vue", "svelte", "swift", "r", "tex", "gd", "gs", "gs2", "gs2json",
    "graal", "gani", "hexa", "srt",
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? static_cast<size_t>(n) : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(wcslen(w.c_str()));
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<size_t>(n) : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    s.resize(strlen(s.c_str()));
    return s;
}

}  // namespace

std::string normalize(const std::string& text) { return narrow(normalizeExtension(widen(text))); }

std::vector<Entry> load() {
    std::set<std::string> all;
    for (const char* e : kDefaultExtensions) all.insert(e);
    for (const std::wstring& name : valueNames(classKey(std::wstring(kAppKey) + L"\SupportedTypes"))) {
        std::string e = narrow(normalizeExtension(name));
        if (!e.empty()) all.insert(e);
    }
    std::vector<Entry> entries;
    for (const std::string& e : all) {
        State s = inspect(widen(e));
        Entry entry;
        entry.ext = e;
        entry.opensWith = narrow(s.description);
        entry.registered = s.registered || s.isDefault;
        entry.checked = entry.registered;
        entries.push_back(entry);
    }
    return entries;
}

void refresh(std::vector<Entry>& entries) {
    for (Entry& e : entries) e.opensWith = narrow(inspect(widen(e.ext)).description);
}

std::string apply(std::vector<Entry>& entries) {
    registerProgId();
    std::wstring notes;
    int on = 0, off = 0;
    for (Entry& e : entries) {
        if (!e.checked && !e.registered) continue;   // never registered and still unticked: nothing to undo
        std::wstring note = applyExtension(widen(e.ext), e.checked);
        if (!note.empty()) notes += (notes.empty() ? L"" : L"  ") + note;
        e.checked ? ++on : ++off;
        e.registered = e.checked;
    }
    notifyShell();
    refresh(entries);
    std::string summary = std::to_string(on) + " registered, " + std::to_string(off) + " released.";
    return notes.empty() ? summary : summary + " " + narrow(notes);
}

void openDefaultAppsSettings() { ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps?registeredAppUser=Moreno%20Text", nullptr, nullptr, SW_SHOWNORMAL); }

}  // namespace FileAssociations

#else

namespace FileAssociations {
std::string normalize(const std::string&) { return {}; }
std::vector<Entry> load() { return {}; }
void refresh(std::vector<Entry>&) {}
std::string apply(std::vector<Entry>&) { return "File associations are only managed on Windows."; }
void openDefaultAppsSettings() {}
}

#endif
