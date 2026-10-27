// Goto Anything: fuzzy file matching over the open tabs and the project folder, with file:line suffixes.
#include "Core/Application.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

bool isSeparator(char c) { return c == '/' || c == '\\' || c == '.' || c == '_' || c == '-' || c == ' '; }

// Case-insensitive subsequence match. Returns -1 when the query does not match, otherwise a score where
// higher is better: consecutive characters, word starts and matches inside the file name all score more.
int fuzzyScore(const std::string& query, const std::string& candidate) {
    if (query.empty()) return 0;
    size_t nameStart = candidate.find_last_of("/\\");
    nameStart = nameStart == std::string::npos ? 0 : nameStart + 1;
    int score = 0;
    size_t ci = 0;
    int run = 0;
    for (size_t qi = 0; qi < query.size(); ++qi) {
        char q = static_cast<char>(std::tolower(static_cast<unsigned char>(query[qi])));
        bool found = false;
        for (; ci < candidate.size(); ++ci) {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate[ci])));
            if (c != q) { run = 0; continue; }
            int bonus = 1;
            if (ci == 0 || isSeparator(candidate[ci - 1])) bonus += 8;
            else if (std::isupper(static_cast<unsigned char>(candidate[ci])) && std::islower(static_cast<unsigned char>(candidate[ci - 1]))) bonus += 6;
            if (ci >= nameStart) bonus += 3;
            if (run > 0) bonus += 6 + run;
            score += bonus;
            ++run;
            ++ci;
            found = true;
            break;
        }
        if (!found) return -1;
    }
    score -= static_cast<int>(candidate.size() - nameStart) / 8;  // prefer short file names
    return score;
}

}  // namespace

void Application::refreshProjectFileList() {
    uint32_t now = SDL_GetTicks();
    if (projectFilesRoot_ == sidebarRoot_ && !projectFiles_.empty() && now - projectFilesAt_ < 8000) return;
    projectFiles_.clear();
    projectFilesRoot_ = sidebarRoot_;
    projectFilesAt_ = now ? now : 1;
    if (sidebarRoot_.empty()) return;
    static const char* skipDirs[] = {".git", ".svn", ".hg", "node_modules", "build", ".vs", "vcpkg_installed", "dist", "out", "target", "obj", "__pycache__"};
    static const char* binaryExts[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".exe", ".dll", ".pdb", ".obj", ".lib", ".o", ".a", ".so", ".zip", ".7z", ".rar", ".gz", ".pyc", ".class", ".jar", ".woff", ".woff2", ".ttf", ".otf", ".mp3", ".mp4", ".wav", ".pdf"};
    std::error_code ec;
    for (fs::recursive_directory_iterator it(sidebarRoot_, fs::directory_options::skip_permission_denied, ec), end; it != end && projectFiles_.size() < 60000; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            std::string name = it->path().filename().string();
            for (auto skip : skipDirs) if (name == skip) { it.disable_recursion_pending(); break; }
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        bool binary = false;
        for (auto b : binaryExts) if (ext == b) { binary = true; break; }
        if (binary) continue;
        projectFiles_.push_back(fs::relative(it->path(), sidebarRoot_, ec).generic_string());
    }
}

void Application::updateFileResults(const std::string& rawQuery) {
    std::string query = rawQuery;
    gotoPendingLine_ = 0;
    size_t colon = query.rfind(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < query.size() &&
        std::all_of(query.begin() + static_cast<std::ptrdiff_t>(colon) + 1, query.end(), [](unsigned char c) { return std::isdigit(c); })) {
        gotoPendingLine_ = static_cast<size_t>(std::strtoull(query.c_str() + colon + 1, nullptr, 10));
        query = query.substr(0, colon);
    }
    struct Hit { int score; std::string label, sub, data; };
    std::vector<Hit> hits;
    std::unordered_set<std::string> openPaths;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const std::string& name = tabs_[i].fileName;
        int score = fuzzyScore(query, name);
        if (score < 0) continue;
        if (!tabs_[i].filePath.empty()) openPaths.insert(tabs_[i].filePath);
        hits.push_back({score + 20, name, tabs_[i].filePath.empty() ? std::string("open tab") : fs::path(tabs_[i].filePath).parent_path().string(), "tab:" + std::to_string(i)});
    }
    refreshProjectFileList();
    for (const std::string& rel : projectFiles_) {
        int score = fuzzyScore(query, rel);
        if (score < 0) continue;
        std::string full = (fs::path(sidebarRoot_) / rel).string();
        if (openPaths.count(full)) continue;
        hits.push_back({score, fs::path(rel).filename().string(), rel, "file:" + full});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    for (size_t i = 0; i < hits.size() && i < 40; ++i) {
        goto_.items.push_back(hits[i].label);
        goto_.subtexts.push_back(hits[i].sub);
        goto_.data.push_back(hits[i].data);
    }
}

void Application::openGotoFile(const std::string& data) {
    recordJump();
    if (data.rfind("tab:", 0) == 0) {
        size_t index = static_cast<size_t>(std::strtoull(data.c_str() + 4, nullptr, 10));
        if (index < tabs_.size()) switchToTab(index);
    } else if (data.rfind("file:", 0) == 0) {
        openFile(data.substr(5));
    }
    if (gotoPendingLine_ > 0) {
        selections_.clear(); selections_.emplace_back(lineStartForLine(gotoPendingLine_ - 1));
        ensureCursorVisible();
    }
    gotoPendingLine_ = 0;
}

// Alt+O: jump between a header and its implementation file.
void Application::switchHeaderImplementation() {
    if (openFilePath_.empty()) return;
    fs::path current = openFilePath_;
    std::string ext = current.extension().string();
    static const std::vector<std::string> headers = {".h", ".hpp", ".hh", ".hxx"};
    static const std::vector<std::string> sources = {".cpp", ".c", ".cc", ".cxx", ".m", ".mm"};
    const auto& wanted = std::find(headers.begin(), headers.end(), ext) != headers.end() ? sources : headers;
    std::error_code ec;
    for (const std::string& candidate : wanted) {
        fs::path sibling = current;
        sibling.replace_extension(candidate);
        if (fs::exists(sibling, ec)) { openFile(sibling.string()); return; }
    }
    refreshProjectFileList();
    std::string stem = current.stem().string();
    for (const std::string& rel : projectFiles_) {
        fs::path p = rel;
        if (p.stem().string() != stem) continue;
        if (std::find(wanted.begin(), wanted.end(), p.extension().string()) != wanted.end()) { openFile((fs::path(sidebarRoot_) / rel).string()); return; }
    }
    pluginStatusText_ = "No matching header/implementation file found";
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}
