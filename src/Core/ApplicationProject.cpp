// Project files (.sublime-project) and text encodings.
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include "Syntax/SyntaxHighlighter.h"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>

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

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

json readJsonFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return json();
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB) text.erase(0, 3);
    static const std::regex trailingComma(R"(,(\s*[}\]]))");
    text = std::regex_replace(text, trailingComma, "$1");
    return json::parse(text, nullptr, false, true);
}

struct EncodingInfo {
    const char* name;
    unsigned codePage;  // 0 = handled manually
    int kind;           // 0 codepage, 1 UTF-16 LE, 2 UTF-16 BE, 3 UTF-8 with BOM
};

const EncodingInfo kEncodings[] = {
    {"UTF-8", 65001, 0}, {"UTF-8 with BOM", 65001, 3}, {"UTF-16 LE", 0, 1}, {"UTF-16 BE", 0, 2},
    {"Western (Windows 1252)", 1252, 0}, {"Western (ISO 8859-1)", 28591, 0}, {"Western (ISO 8859-15)", 28605, 0}, {"Western (Mac Roman)", 10000, 0},
    {"Central European (Windows 1250)", 1250, 0}, {"Central European (ISO 8859-2)", 28592, 0},
    {"Cyrillic (Windows 1251)", 1251, 0}, {"Cyrillic (KOI8-R)", 20866, 0}, {"Cyrillic (KOI8-U)", 21866, 0}, {"Cyrillic (ISO 8859-5)", 28595, 0}, {"Cyrillic (Windows 866)", 866, 0},
    {"Greek (Windows 1253)", 1253, 0}, {"Greek (ISO 8859-7)", 28597, 0}, {"Turkish (Windows 1254)", 1254, 0}, {"Turkish (ISO 8859-9)", 28599, 0},
    {"Hebrew (Windows 1255)", 1255, 0}, {"Arabic (Windows 1256)", 1256, 0}, {"Baltic (Windows 1257)", 1257, 0}, {"Vietnamese (Windows 1258)", 1258, 0},
    {"DOS (CP 437)", 437, 0}, {"Chinese (GB18030)", 54936, 0}, {"Japanese (Shift JIS)", 932, 0}, {"Korean (EUC-KR)", 51949, 0},
};

const EncodingInfo* findEncoding(const std::string& name) {
    for (auto& e : kEncodings) if (name == e.name) return &e;
    return nullptr;
}

bool decodeBytes(const std::string& bytes, const EncodingInfo& enc, std::string& out) {
    if (enc.kind == 1 || enc.kind == 2) {
        size_t pos = (bytes.size() >= 2 && ((enc.kind == 1 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) ||
                                            (enc.kind == 2 && (unsigned char)bytes[0] == 0xFE && (unsigned char)bytes[1] == 0xFF))) ? 2 : 0;
        std::u16string wide;
        for (; pos + 1 < bytes.size(); pos += 2) {
            unsigned char a = (unsigned char)bytes[pos], b = (unsigned char)bytes[pos + 1];
            wide += static_cast<char16_t>(enc.kind == 1 ? (a | (b << 8)) : ((a << 8) | b));
        }
        out.clear();
        for (size_t i = 0; i < wide.size(); ++i) {
            char32_t cp = wide[i];
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 && wide[i + 1] < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (wide[i + 1] - 0xDC00); ++i; }
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        }
        return true;
    }
    if (enc.codePage == 65001) {
        out = bytes;
        if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
        return true;
    }
#ifdef _WIN32
    if (bytes.empty()) { out.clear(); return true; }
    int wideLen = MultiByteToWideChar(enc.codePage, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (wideLen <= 0) return false;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(enc.codePage, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(), wideLen);
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    out.assign(static_cast<size_t>(utf8Len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, out.data(), utf8Len, nullptr, nullptr);
    return true;
#else
    return false;
#endif
}

bool encodeText(const std::string& utf8, const EncodingInfo& enc, std::string& out) {
    if (enc.kind == 3) { out = "\xEF\xBB\xBF" + utf8; return true; }
    if (enc.kind == 0 && enc.codePage == 65001) { out = utf8; return true; }
    if (enc.kind == 1 || enc.kind == 2) {
        out = enc.kind == 1 ? std::string("\xFF\xFE", 2) : std::string("\xFE\xFF", 2);
        auto put = [&](unsigned unit) {
            if (enc.kind == 1) { out += static_cast<char>(unit & 0xFF); out += static_cast<char>(unit >> 8); }
            else { out += static_cast<char>(unit >> 8); out += static_cast<char>(unit & 0xFF); }
        };
        for (size_t i = 0; i < utf8.size();) {
            unsigned char c = (unsigned char)utf8[i];
            char32_t cp; size_t n;
            if (c < 0x80) { cp = c; n = 1; }
            else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
            else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
            else { cp = c & 0x07; n = 4; }
            for (size_t k = 1; k < n && i + k < utf8.size(); ++k) cp = (cp << 6) | ((unsigned char)utf8[i + k] & 0x3F);
            i += n;
            if (cp >= 0x10000) { cp -= 0x10000; put(0xD800 + (cp >> 10)); put(0xDC00 + (cp & 0x3FF)); }
            else put(static_cast<unsigned>(cp));
        }
        return true;
    }
#ifdef _WIN32
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(std::max(0, wideLen)), L'\0');
    if (wideLen > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLen);
    int len = WideCharToMultiByte(enc.codePage, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
    out.assign(static_cast<size_t>(std::max(0, len)), '\0');
    if (len > 0) WideCharToMultiByte(enc.codePage, 0, wide.data(), wideLen, out.data(), len, nullptr, nullptr);
    return true;
#else
    return false;
#endif
}

}  // namespace

// ---- Encodings ---------------------------------------------------------------------------------

void Application::updateEncodingResults(const std::string& query) {
    // query: "R" or "S" followed by a filter; R reopens the file, S saves it
    if (query.empty()) return;
    char mode = query[0];
    std::string needle = lowered(query.substr(1));
    std::string current = fileEncodings_.count(openFilePath_) ? fileEncodings_[openFilePath_] : "UTF-8";
    for (auto& e : kEncodings) {
        if (!needle.empty() && lowered(e.name).find(needle) == std::string::npos) continue;
        goto_.items.push_back(std::string(e.name) + (current == e.name ? "  (current)" : ""));
        goto_.subtexts.push_back(mode == 'R' ? "Reopen file" : "Save file");
        goto_.data.push_back(std::string(1, mode) + e.name);
    }
}

void Application::openReopenEncodingPicker() {
    if (openFilePath_.empty()) { pluginStatusText_ = "Save the file first to reopen it with another encoding"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
    openGotoWithPrefix("*R");
}

void Application::openSaveEncodingPicker() { openGotoWithPrefix("*S"); }

void Application::applyEncodingChoice(const std::string& data) {
    if (data.size() < 2) return;
    const EncodingInfo* enc = findEncoding(data.substr(1));
    if (!enc) return;
    if (data[0] == 'R') reopenWithEncoding(enc->name); else saveWithEncoding(enc->name);
}

void Application::reopenWithEncoding(const std::string& name) {
    const EncodingInfo* enc = findEncoding(name);
    if (!enc || openFilePath_.empty() || largeFileGuarded_ || binaryView_) return;
    std::ifstream in(openFilePath_, std::ios::binary);
    if (!in) return;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string text;
    if (!decodeBytes(bytes, *enc, text)) { pluginStatusText_ = "This encoding isn't supported on this platform"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
    pushUndo();
    textBuffer = std::move(text);
    selections_.clear(); selections_.emplace_back(0);
    fileEncodings_[openFilePath_] = enc->name;
    invalidateLineIndex(); dirty_ = false; syntaxDirty_ = true;
    if (activeTab_ < tabs_.size()) { tabs_[activeTab_].text = textBuffer; tabs_[activeTab_].dirty = false; }
    pluginStatusText_ = std::string("Reopened as ") + enc->name;
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

void Application::saveWithEncoding(const std::string& name) {
    const EncodingInfo* enc = findEncoding(name);
    if (!enc || largeFileGuarded_ || binaryView_) return;
    if (openFilePath_.empty()) saveFileAs();
    if (openFilePath_.empty()) return;
    fileEncodings_[openFilePath_] = enc->name;
    saveFile();
    pluginStatusText_ = std::string("Saved as ") + enc->name;
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

std::string Application::encodeForSave(const std::string& path, const std::string& utf8) const {
    auto it = fileEncodings_.find(path);
    if (it == fileEncodings_.end() || it->second == "UTF-8") return utf8;
    const EncodingInfo* enc = findEncoding(it->second);
    std::string out;
    if (enc && encodeText(utf8, *enc, out)) return out;
    return utf8;
}

// ---- Projects ----------------------------------------------------------------------------------

namespace {

void pushRecentProject(const std::string& path) {
    auto& settings = SettingsManager::instance();
    auto recent = settings.get<std::vector<std::string>>("recent_projects", std::vector<std::string>());
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > 15) recent.resize(15);
    settings.set("recent_projects", recent);
    settings.save();
}

}  // namespace

void Application::openProjectFile(const std::string& path) {
    json project = readJsonFile(path);
    if (!project.is_object()) { pluginStatusText_ = "Couldn't read the project file"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
    fs::path base = fs::path(path).parent_path();
    if (project.contains("folders") && project["folders"].is_array() && !project["folders"].empty()) {
        auto& first = project["folders"][0];
        std::string folder = first.is_object() ? first.value("path", std::string(".")) : (first.is_string() ? first.get<std::string>() : std::string("."));
        fs::path resolved = fs::path(folder).is_absolute() ? fs::path(folder) : base / folder;
        std::error_code ec;
        resolved = fs::weakly_canonical(resolved, ec);
        if (fs::is_directory(resolved)) { loadFolder(resolved.string()); sidebarVisible_ = true; }
    }
    if (project.contains("settings") && project["settings"].is_object()) {
        auto& st = project["settings"];
        if (st.contains("tab_size") && st["tab_size"].is_number_integer()) { tabSize_ = std::clamp(st["tab_size"].get<int>(), 1, 16); syntax_->setTabSize(tabSize_); }
        if (st.contains("translate_tabs_to_spaces") && st["translate_tabs_to_spaces"].is_boolean()) { useTabs_ = !st["translate_tabs_to_spaces"].get<bool>(); syntax_->setUseTabs(useTabs_); }
        if (st.contains("rulers") && st["rulers"].is_array()) { rulers_.clear(); for (auto& r : st["rulers"]) if (r.is_number_integer()) rulers_.push_back(r.get<int>()); }
        if (st.contains("word_wrap") && st["word_wrap"].is_boolean()) { wordWrap_ = st["word_wrap"].get<bool>(); visualLines_.clear(); }
    }
    projectFile_ = path;
    pushRecentProject(path);
    pluginStatusText_ = "Project: " + fs::path(path).stem().string();
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

void Application::openProject() {
#ifdef _WIN32
    closeAllPopups();
    inOsDialog_ = true;
    char buf[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Project Files\0*.sublime-project;*.moreno-project\0All Files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
    bool picked = GetOpenFileNameA(&ofn) != 0;
    inOsDialog_ = false;
    if (picked) openProjectFile(buf);
#endif
}

void Application::saveProjectAs() {
#ifdef _WIN32
    closeAllPopups();
    inOsDialog_ = true;
    char buf[MAX_PATH] = {};
    std::string suggestion = sidebarRoot_.empty() ? "project" : fs::path(sidebarRoot_).filename().string();
    std::snprintf(buf, sizeof(buf), "%s", suggestion.c_str());
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Project Files\0*.sublime-project\0All Files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_OVERWRITEPROMPT; ofn.lpstrDefExt = "sublime-project";
    bool picked = GetSaveFileNameA(&ofn) != 0;
    inOsDialog_ = false;
    if (!picked) return;
    fs::path file = buf;
    json project = fs::exists(file) ? readJsonFile(file) : json::object();
    if (!project.is_object()) project = json::object();
    std::error_code ec;
    std::string folderPath = sidebarRoot_.empty() ? std::string(".") : fs::relative(sidebarRoot_, file.parent_path(), ec).generic_string();
    if (ec || folderPath.empty()) folderPath = sidebarRoot_;
    project["folders"] = json::array({json{{"path", folderPath}}});
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << project.dump(4);
    projectFile_ = file.string();
    pushRecentProject(projectFile_);
    pluginStatusText_ = "Saved project: " + file.filename().string();
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
#endif
}

void Application::editProject() {
    if (projectFile_.empty()) {
        if (sidebarRoot_.empty()) { pluginStatusText_ = "Open a folder first"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
        fs::path file = fs::path(sidebarRoot_) / (fs::path(sidebarRoot_).filename().string() + ".sublime-project");
        if (!fs::exists(file)) {
            std::ofstream out(file, std::ios::binary);
            out << "{\n\t\"folders\":\n\t[\n\t\t{\n\t\t\t\"path\": \".\"\n\t\t}\n\t]\n}\n";
        }
        projectFile_ = file.string();
        pushRecentProject(projectFile_);
    }
    openFile(projectFile_);
}

void Application::closeProject() {
    closeFolder();
    projectFile_.clear();
}

void Application::openRecentProjectPicker() { openGotoWithPrefix("^"); }

void Application::updateProjectResults(const std::string& pattern) {
    std::string needle = lowered(pattern);
    auto recent = SettingsManager::instance().get<std::vector<std::string>>("recent_projects", std::vector<std::string>());
    for (auto& path : recent) {
        std::string name = fs::path(path).stem().string();
        if (!needle.empty() && lowered(name).find(needle) == std::string::npos && lowered(path).find(needle) == std::string::npos) continue;
        goto_.items.push_back(name);
        goto_.subtexts.push_back(path);
        goto_.data.push_back(path);
    }
}
