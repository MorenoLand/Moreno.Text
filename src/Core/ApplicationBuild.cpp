// Build systems (.sublime-build files), build output, and the project-wide symbol index behind
// Goto Symbol in Project, Goto Definition and Goto Reference.
#include "Core/Application.h"
#include "Syntax/SyntaxHighlighter.h"
#include <nlohmann/json.hpp>
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

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
using json = nlohmann::json;

namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string platformKey() {
#ifdef _WIN32
    return "windows";
#elif defined(__APPLE__)
    return "osx";
#else
    return "linux";
#endif
}

json readRelaxedJson(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return json();
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB) text.erase(0, 3);
    static const std::regex trailingComma(R"(,(\s*[}\]]))");
    text = std::regex_replace(text, trailingComma, "$1");
    return json::parse(text, nullptr, false, true);
}

std::string expandVariables(const std::string& in, const std::map<std::string, std::string>& vars) {
    static const std::regex re(R"(\$\{(\w+)(?::([^}]*))?\}|\$(\w+))");
    std::string out;
    auto begin = std::sregex_iterator(in.begin(), in.end(), re);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out.append(in, last, static_cast<size_t>(it->position()) - last);
        std::string key = (*it)[1].matched ? (*it)[1].str() : (*it)[3].str();
        auto found = vars.find(key);
        if (found != vars.end()) out += found->second;
        else if ((*it)[2].matched) out += (*it)[2].str();
        else out += it->str();
        last = static_cast<size_t>(it->position() + it->length());
    }
    out.append(in, last, std::string::npos);
    return out;
}

std::string quoteArg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    std::string out = "\"";
    for (char c : arg) { if (c == '"') out += '\\'; out += c; }
    return out + "\"";
}

}  // namespace

struct Application::BuildJob {
    std::mutex mutex;
    std::string pending;
    bool done = false;
    int exitCode = 0;
    bool cancelled = false;
    std::thread worker;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::string fileRegex, workDir;
#ifdef _WIN32
    HANDLE process = nullptr;
#endif
    ~BuildJob() {
        cancel();
        if (worker.joinable()) worker.join();
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex);
        cancelled = true;
#ifdef _WIN32
        if (process) TerminateProcess(process, 1);
#endif
    }
};

struct BuildChoice {
    std::string label, systemName;
    json spec;
    std::string path;
};

static std::vector<BuildChoice> collectBuildChoices(const std::vector<std::string>& roots) {
    std::vector<BuildChoice> out;
    for (auto& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                std::string name = it->path().filename().string();
                if (name == ".git" || name == "node_modules" || name == "build") it.disable_recursion_pending();
                continue;
            }
            if (it->path().extension() != ".sublime-build") continue;
            json spec = readRelaxedJson(it->path());
            if (!spec.is_object()) continue;
            std::string name = it->path().stem().string();
            auto base = spec;
            auto platform = base.find(platformKey());
            if (platform != base.end() && platform->is_object()) for (auto& [k, v] : platform->items()) base[k] = v;
            out.push_back({name, name, base, it->path().string()});
            if (base.contains("variants") && base["variants"].is_array()) {
                for (auto& variant : base["variants"]) {
                    if (!variant.is_object()) continue;
                    json merged = base;
                    merged.erase("variants");
                    for (auto& [k, v] : variant.items()) merged[k] = v;
                    auto vp = variant.find(platformKey());
                    if (vp != variant.end() && vp->is_object()) for (auto& [k, v] : vp->items()) merged[k] = v;
                    out.push_back({name + " - " + variant.value("name", std::string("Variant")), name, merged, it->path().string()});
                }
            }
        }
    }
    return out;
}

static std::vector<BuildChoice> allBuildChoices(const AppPaths& paths, const std::string& projectRoot) {
    std::vector<std::string> roots = {paths.packagesDir, (fs::path(paths.cacheDir) / "Installed Packages").string(), paths.installedPackagesDir};
    std::vector<BuildChoice> choices = collectBuildChoices(roots);
    if (!projectRoot.empty()) {
        std::error_code ec;
        for (fs::directory_iterator it(projectRoot, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->path().extension() != ".sublime-build") continue;
            json spec = readRelaxedJson(it->path());
            if (spec.is_object()) choices.insert(choices.begin(), {it->path().stem().string(), it->path().stem().string(), spec, it->path().string()});
        }
    }
    return choices;
}

static bool selectorMatches(const std::string& selector, const std::string& language, const std::string& extension) {
    std::string lang = lowered(language);
    std::string sel = lowered(selector);
    if (sel.empty()) return false;
    std::string normalized;
    for (char c : lang) normalized += (c == ' ') ? '-' : c;
    size_t dot = sel.rfind('.');
    std::string tail = dot == std::string::npos ? sel : sel.substr(dot + 1);
    if (tail == lang || tail == normalized) return true;
    if (!extension.empty() && (tail == lowered(extension) || (tail == "js" && extension == "js"))) return true;
    return sel.find("." + lang) != std::string::npos || sel.find("." + normalized) != std::string::npos;
}

void Application::cancelBuild() {
    if (buildJob_) {
        buildJob_->cancel();
        pluginStatusText_ = "Build cancelled";
        pluginStatusUntil_ = SDL_GetTicks() + 3000;
    }
}

void Application::appendToBuildTab(const std::string& chunk) {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].fileName != "Build Results") continue;
        if (i == activeTab_) { textBuffer += chunk; invalidateLineIndex(); }
        else tabs_[i].text += chunk;
        return;
    }
}

void Application::showBuildTab() {
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].fileName == "Build Results") {
            switchToTab(i);
            textBuffer.clear(); invalidateLineIndex();
            selections_.clear(); selections_.emplace_back(0);
            return;
        }
    }
    newBuffer();
    openFile_ = "Build Results";
    if (activeTab_ < tabs_.size()) tabs_[activeTab_].fileName = "Build Results";
    dirty_ = false;
}

void Application::indexBuildResults() {
    findHits_.clear(); findResultLines_.clear(); findHitCursor_ = 0;
    if (buildFileRegex_.empty()) return;
    std::regex re;
    try { re = std::regex(buildFileRegex_, std::regex::ECMAScript); } catch (...) { return; }
    size_t lineNo = 0, pos = 0;
    while (pos <= textBuffer.size()) {
        size_t end = textBuffer.find('\n', pos);
        if (end == std::string::npos) end = textBuffer.size();
        std::string line = textBuffer.substr(pos, end - pos);
        std::smatch m;
        if (std::regex_search(line, m, re) && m.size() >= 3 && m[1].matched) {
            fs::path file = m[1].str();
            if (file.is_relative()) file = fs::path(buildWorkDir_) / file;
            size_t target = m[2].matched ? static_cast<size_t>(std::strtoull(m[2].str().c_str(), nullptr, 10)) : 0;
            findResultLines_.emplace_back(lineNo, findHits_.size());
            findHits_.emplace_back(file.string(), target);
        }
        ++lineNo;
        if (end >= textBuffer.size()) break;
        pos = end + 1;
    }
}

void Application::pollBuild() {
    if (!buildJob_) return;
    std::string chunk;
    bool finished = false;
    int exitCode = 0;
    bool cancelled = false;
    double seconds = 0;
    {
        std::lock_guard<std::mutex> lock(buildJob_->mutex);
        chunk.swap(buildJob_->pending);
        finished = buildJob_->done;
        exitCode = buildJob_->exitCode;
        cancelled = buildJob_->cancelled;
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - buildJob_->started).count();
    }
    if (!chunk.empty()) appendToBuildTab(chunk);
    if (!finished) return;
    char timing[64];
    std::snprintf(timing, sizeof(timing), "%.1fs", seconds);
    std::string tail = cancelled ? "\n[Build cancelled]\n" : (exitCode == 0 ? "\n[Finished in " + std::string(timing) + "]\n"
                                                                            : "\n[Finished in " + std::string(timing) + " with exit code " + std::to_string(exitCode) + "]\n");
    appendToBuildTab(tail);
    buildJob_.reset();
    for (size_t i = 0; i < tabs_.size(); ++i) if (tabs_[i].fileName == "Build Results" && i == activeTab_) { indexBuildResults(); break; }
    pluginStatusText_ = cancelled ? "Build cancelled" : (exitCode == 0 ? "Build finished" : "Build failed (exit code " + std::to_string(exitCode) + ")");
    pluginStatusUntil_ = SDL_GetTicks() + 4000;
}

static void runProcess(std::shared_ptr<Application::BuildJob> job, std::string commandLine, std::string workDir, std::map<std::string, std::string> env);

void Application::runBuildChoice(size_t choiceIndex) {
    auto choices = allBuildChoices(paths_, sidebarRoot_);
    if (choiceIndex >= choices.size()) return;
    startBuild(choices[choiceIndex].spec, choices[choiceIndex].label);
}

void Application::runBuild() {
    if (buildJob_) { cancelBuild(); return; }
    auto choices = allBuildChoices(paths_, sidebarRoot_);
    std::string ext = fs::path(openFilePath_).extension().string();
    if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
    const BuildChoice* pick = nullptr;
    if (!buildSystemName_.empty()) {
        for (auto& c : choices) if (c.label == buildSystemName_) { pick = &c; break; }
    }
    if (!pick) {
        for (auto& c : choices) {
            if (c.label != c.systemName) continue;
            if (selectorMatches(c.spec.value("selector", std::string()), syntax_->languageName(), ext)) { pick = &c; break; }
        }
    }
    if (!pick) {
        pluginStatusText_ = choices.empty() ? "No build systems found (add a .sublime-build file)" : "No build system matches this file";
        pluginStatusUntil_ = SDL_GetTicks() + 4000;
        return;
    }
    startBuild(pick->spec, pick->label);
}

void Application::startBuild(const nlohmann::json& spec, const std::string& label) {
    saveAll();
    std::map<std::string, std::string> vars;
    fs::path file = openFilePath_;
    if (!openFilePath_.empty()) {
        vars["file"] = file.string(); vars["file_path"] = file.parent_path().string(); vars["file_name"] = file.filename().string();
        vars["file_base_name"] = file.stem().string(); vars["file_extension"] = file.extension().string().empty() ? "" : file.extension().string().substr(1);
    }
    if (!sidebarRoot_.empty()) { vars["folder"] = sidebarRoot_; vars["project_path"] = sidebarRoot_; vars["project"] = sidebarRoot_; }
    vars["packages"] = paths_.packagesDir; vars["platform"] = platformKey() == "windows" ? "Windows" : platformKey() == "osx" ? "OSX" : "Linux";

    std::string commandLine;
    if (spec.contains("shell_cmd") && spec["shell_cmd"].is_string()) {
#ifdef _WIN32
        commandLine = "cmd.exe /C " + expandVariables(spec["shell_cmd"].get<std::string>(), vars);
#else
        commandLine = "/bin/sh -c " + quoteArg(expandVariables(spec["shell_cmd"].get<std::string>(), vars));
#endif
    } else if (spec.contains("cmd")) {
        if (spec["cmd"].is_string()) commandLine = expandVariables(spec["cmd"].get<std::string>(), vars);
        else if (spec["cmd"].is_array()) {
            for (auto& part : spec["cmd"]) { if (!commandLine.empty()) commandLine += ' '; commandLine += quoteArg(expandVariables(part.get<std::string>(), vars)); }
            if (spec.value("shell", false)) {
#ifdef _WIN32
                commandLine = "cmd.exe /C " + commandLine;
#else
                commandLine = "/bin/sh -c " + quoteArg(commandLine);
#endif
            }
        }
    }
    if (commandLine.empty()) {
        pluginStatusText_ = "Build system \"" + label + "\" has no command";
        pluginStatusUntil_ = SDL_GetTicks() + 4000;
        return;
    }
    std::string workDir = spec.contains("working_dir") && spec["working_dir"].is_string() ? expandVariables(spec["working_dir"].get<std::string>(), vars)
                          : (!openFilePath_.empty() ? file.parent_path().string() : (!sidebarRoot_.empty() ? sidebarRoot_ : fs::current_path().string()));
    std::map<std::string, std::string> env;
    if (spec.contains("env") && spec["env"].is_object()) for (auto& [k, v] : spec["env"].items()) if (v.is_string()) env[k] = expandVariables(v.get<std::string>(), vars);
    if (spec.contains("path") && spec["path"].is_string()) {
        const char* current = std::getenv("PATH");
#ifdef _WIN32
        env["PATH"] = expandVariables(spec["path"].get<std::string>(), vars) + ";" + (current ? current : "");
#else
        env["PATH"] = expandVariables(spec["path"].get<std::string>(), vars) + ":" + (current ? current : "");
#endif
    }
    buildFileRegex_ = spec.value("file_regex", std::string());
    buildWorkDir_ = workDir;

    showBuildTab();
    textBuffer = "[Cmd: " + commandLine + "]\n[Dir: " + workDir + "]\n\n";
    invalidateLineIndex();
    selections_.clear(); selections_.emplace_back(textBuffer.size());
    buildJob_ = std::make_shared<BuildJob>();
    buildJob_->fileRegex = buildFileRegex_; buildJob_->workDir = workDir;
    pluginStatusText_ = "Building: " + label;
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
    auto job = buildJob_;
    job->worker = std::thread(runProcess, job, commandLine, workDir, env);
}

static void runProcess(std::shared_ptr<Application::BuildJob> job, std::string commandLine, std::string workDir, std::map<std::string, std::string> env) {
    auto push = [&](const char* data, size_t n) {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->pending.append(data, n);
    };
#ifdef _WIN32
    for (auto& [k, v] : env) SetEnvironmentVariableA(k.c_str(), v.c_str());
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) { std::lock_guard<std::mutex> lock(job->mutex); job->pending = "Failed to create output pipe\n"; job->exitCode = 1; job->done = true; return; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe; si.hStdError = writePipe; si.hStdInput = nul;
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        std::string msg = "Failed to start process (error " + std::to_string(GetLastError()) + ")\n";
        push(msg.data(), msg.size());
        CloseHandle(readPipe);
        std::lock_guard<std::mutex> lock(job->mutex); job->exitCode = 1; job->done = true;
        return;
    }
    { std::lock_guard<std::mutex> lock(job->mutex); job->process = pi.hProcess; }
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) push(buffer, got);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(readPipe);
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->process = nullptr;
        job->exitCode = static_cast<int>(code);
        job->done = true;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    for (auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
    std::string full = "cd " + quoteArg(workDir) + " && " + commandLine + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) { std::lock_guard<std::mutex> lock(job->mutex); job->pending = "Failed to start process\n"; job->exitCode = 1; job->done = true; return; }
    char buffer[4096];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) push(buffer, n);
    int status = pclose(pipe);
    std::lock_guard<std::mutex> lock(job->mutex);
    job->exitCode = status;
    job->done = true;
#endif
}

void Application::openBuildPicker() {
    openGotoWithPrefix("!");
}

// ---- Project symbols --------------------------------------------------------------------------

struct ProjectSymbol {
    std::string name, path, kind;
    size_t line;
};

static std::vector<ProjectSymbol> scanProjectSymbols(const std::string& root) {
    static const std::regex keywordDef(R"(^\s*(?:export\s+|public\s+|private\s+|protected\s+|static\s+|async\s+|pub\s+|abstract\s+|final\s+|default\s+)*(function|def|class|struct|enum|interface|trait|fn|func|impl|type|namespace|module|object|record|union|macro_rules!)\s+([A-Za-z_][\w$]*))");
    static const std::regex cFunction(R"(^[A-Za-z_][\w:<>\*&\s,]*?[\s\*&]([A-Za-z_~][\w:]*)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{?\s*$)");
    static const char* sourceExts[] = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh", ".cs", ".java", ".js", ".jsx", ".ts", ".tsx", ".py", ".rb", ".go", ".rs", ".php", ".lua", ".swift", ".kt", ".scala", ".gd", ".pas", ".sh"};
    static const char* skipDirs[] = {".git", ".svn", ".hg", "node_modules", "build", ".vs", "vcpkg_installed", "dist", "out", "target", "bin", "obj"};
    std::vector<ProjectSymbol> out;
    std::error_code ec;
    size_t filesSeen = 0;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && out.size() < 40000 && filesSeen < 6000; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
            std::string name = it->path().filename().string();
            for (auto skip : skipDirs) if (name == skip) { it.disable_recursion_pending(); break; }
            continue;
        }
        std::string ext = lowered(it->path().extension().string());
        bool known = false;
        for (auto e : sourceExts) if (ext == e) { known = true; break; }
        if (!known || it->file_size(ec) > 2u * 1024u * 1024u) continue;
        ++filesSeen;
        std::ifstream in(it->path(), std::ios::binary);
        std::string line;
        size_t lineNo = 0;
        bool cLike = ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".cs" || ext == ".java";
        while (std::getline(in, line)) {
            ++lineNo;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 400) continue;
            std::smatch m;
            if (std::regex_search(line, m, keywordDef)) out.push_back({m[2].str(), it->path().string(), m[1].str(), lineNo});
            else if (cLike && !line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '#' && std::regex_match(line, m, cFunction)) {
                std::string name = m[1].str();
                if (name != "if" && name != "for" && name != "while" && name != "switch" && name != "return" && name != "else")
                    out.push_back({name, it->path().string(), "function", lineNo});
            }
        }
    }
    return out;
}

void Application::updateProjectSymbolResults(const std::string& pattern) {
    if (sidebarRoot_.empty()) return;
    uint32_t now = SDL_GetTicks();
    if (projectSymbolNames_.empty() || now - projectSymbolsBuiltAt_ > 15000 || projectSymbolsRoot_ != sidebarRoot_) {
        projectSymbolNames_.clear(); projectSymbolPaths_.clear(); projectSymbolLines_.clear();
        for (auto& sym : scanProjectSymbols(sidebarRoot_)) {
            projectSymbolNames_.push_back(sym.name + "  (" + sym.kind + ")");
            projectSymbolPaths_.push_back(sym.path);
            projectSymbolLines_.push_back(sym.line);
        }
        projectSymbolsBuiltAt_ = now; projectSymbolsRoot_ = sidebarRoot_;
    }
    std::string needle = lowered(pattern);
    std::error_code ec;
    for (size_t i = 0; i < projectSymbolNames_.size() && goto_.items.size() < 200; ++i) {
        if (!needle.empty() && lowered(projectSymbolNames_[i]).find(needle) == std::string::npos) continue;
        goto_.items.push_back(projectSymbolNames_[i]);
        goto_.subtexts.push_back(fs::relative(projectSymbolPaths_[i], sidebarRoot_, ec).generic_string() + ":" + std::to_string(projectSymbolLines_[i]));
        goto_.data.push_back(projectSymbolPaths_[i] + "|" + std::to_string(projectSymbolLines_[i]));
    }
}

void Application::updateBuildPickerResults(const std::string& pattern) {
    auto choices = allBuildChoices(paths_, sidebarRoot_);
    std::string needle = lowered(pattern);
    for (size_t i = 0; i < choices.size(); ++i) {
        if (!needle.empty() && lowered(choices[i].label).find(needle) == std::string::npos) continue;
        goto_.items.push_back(choices[i].label);
        goto_.subtexts.push_back(fs::path(choices[i].path).filename().string());
        goto_.data.push_back(std::to_string(i));
    }
}

void Application::openLocation(const std::string& path, size_t line) {
    recordJump();
    openFile(path);
    if (line > 0) {
        selections_.clear(); selections_.emplace_back(lineStartForLine(line - 1));
        ensureCursorVisible();
    }
}

void Application::gotoDefinition() {
    std::string word = wordOrSelectionAtCursor();
    if (word.empty()) return;
    if (sidebarRoot_.empty()) { pluginStatusText_ = "Goto Definition needs an open folder"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
    auto symbols = scanProjectSymbols(sidebarRoot_);
    std::vector<const ProjectSymbol*> hits;
    for (auto& s : symbols) if (s.name == word) hits.push_back(&s);
    if (hits.size() == 1) { openLocation(hits[0]->path, hits[0]->line); return; }
    if (hits.empty()) { pluginStatusText_ = "Definition of \"" + word + "\" not found"; pluginStatusUntil_ = SDL_GetTicks() + 3000; return; }
    openGotoWithPrefix("@@" + word);
}

void Application::gotoReference() {
    std::string word = wordOrSelectionAtCursor();
    if (word.empty() || sidebarRoot_.empty()) return;
    find_.query = word; find_.regex = false; find_.wholeWord = true;
    runFindInFiles();
}
