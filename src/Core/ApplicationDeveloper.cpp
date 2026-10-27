// Developer menu: package templates, package file browsing, HTML export and printing.
#include "Core/Application.h"
#include "Syntax/SyntaxHighlighter.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string escapeHtml(const std::string& text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

}  // namespace

void Application::createFromTemplate(const std::string& stem, const std::string& extension, const std::string& contents) {
    fs::path dir = fs::path(paths_.packagesDir) / "User";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file;
    for (int n = 0; n < 1000; ++n) {
        file = dir / (stem + (n ? std::to_string(n + 1) : std::string()) + extension);
        if (!fs::exists(file, ec)) break;
    }
    { std::ofstream out(file, std::ios::binary); out << contents; }
    openFile(file.string());
}

void Application::newPlugin() {
    createFromTemplate("plugin", ".py",
        "import sublime\nimport sublime_plugin\n\n\nclass ExampleCommand(sublime_plugin.TextCommand):\n    def run(self, edit):\n        self.view.insert(edit, 0, \"Hello, World!\")\n");
}

void Application::newSnippet() {
    createFromTemplate("snippet", ".sublime-snippet",
        "<snippet>\n\t<content><![CDATA[\nHello, ${1:this} is a ${2:snippet}.\n]]></content>\n\t<!-- Optional: Set a tabTrigger to define how to trigger the snippet -->\n"
        "\t<!-- <tabTrigger>hello</tabTrigger> -->\n\t<!-- Optional: Set a scope to limit where the snippet will trigger -->\n\t<!-- <scope>source.python</scope> -->\n</snippet>\n");
}

void Application::newSyntax() {
    createFromTemplate("syntax", ".sublime-syntax",
        "%YAML 1.2\n---\n# See http://www.sublimetext.com/docs/syntax.html\nname: Syntax Name\nfile_extensions:\n  - ext\nscope: source.example\n\ncontexts:\n  main:\n"
        "    # Strings begin and end with quotes, and use backslashes as an escape character\n    - match: '\"'\n      scope: punctuation.definition.string.begin.example\n      push: double_quoted_string\n\n"
        "    # Comments begin with a '//' and finish at the end of the line\n    - match: '//'\n      scope: punctuation.definition.comment.example\n      push: line_comment\n\n"
        "    # Keywords are if, else for and while.\n    - match: '\\b(if|else|for|while)\\b'\n      scope: keyword.control.example\n\n"
        "    # Numbers\n    - match: '\\b(-)?[0-9.]+\\b'\n      scope: constant.numeric.example\n\n"
        "  double_quoted_string:\n    - meta_include_prototype: false\n    - meta_scope: string.quoted.double.example\n    - match: '\\\\.'\n      scope: constant.character.escape.example\n"
        "    - match: '\"'\n      scope: punctuation.definition.string.end.example\n      pop: true\n\n"
        "  line_comment:\n    - meta_include_prototype: false\n    - meta_scope: comment.line.example\n    - match: $\n      pop: true\n");
}

void Application::newBuildSystem() {
    createFromTemplate("build", ".sublime-build",
        "{\n\t\"shell_cmd\": \"make\"\n}\n");
}

void Application::openMouseBindings() {
    fs::path dir = fs::path(paths_.packagesDir) / "User";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / "Default (Windows).sublime-mousemap";
    if (!fs::exists(file, ec)) { std::ofstream out(file, std::ios::binary); out << "[\n\t// Add mouse bindings here\n]\n"; }
    openFile(file.string());
}

void Application::browsePackages() {
    std::string dir = paths_.packagesDir;
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + dir + "' &";
    std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open '" + dir + "' &";
    std::system(cmd.c_str());
#endif
}

void Application::openPackageFilePicker() { openGotoWithPrefix("~"); }

void Application::updatePackageFileResults(const std::string& pattern) {
    std::string needle = lowered(pattern);
    std::vector<std::string> roots = {paths_.packagesDir, (fs::path(paths_.cacheDir) / "Installed Packages").string()};
    std::error_code ec;
    for (auto& root : roots) {
        if (!fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end && goto_.items.size() < 300; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string rel = fs::relative(it->path(), root, ec).generic_string();
            if (!needle.empty() && lowered(rel).find(needle) == std::string::npos) continue;
            goto_.items.push_back(rel);
            goto_.subtexts.push_back(root == paths_.packagesDir ? "Packages" : "Installed Packages");
            goto_.data.push_back(it->path().string());
        }
    }
}

// ---- HTML export -----------------------------------------------------------------------------

std::string Application::exportToHtml(bool wholeDocument) {
    size_t a = 0, b = textBuffer.size();
    if (!wholeDocument && !selections_.empty() && selections_[0].hasSelection()) { a = selections_[0].min(); b = selections_[0].max(); }
    auto toHex = [](float v) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", static_cast<int>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f));
        return std::string(buf);
    };
    auto bg = syntax_->backgroundColor();
    auto fg = syntax_->scopeColor(0);
    std::ostringstream html;
    html << "<div class=\"ST\" style=\"background-color:#" << toHex(bg.r) << toHex(bg.g) << toHex(bg.b) << ";color:#" << toHex(fg.r) << toHex(fg.g) << toHex(fg.b)
         << ";font-family:Consolas,monospace;white-space:pre;padding:6px;\">";
    auto emit = [&](size_t from, size_t to, int scope) {
        if (to <= from) return;
        auto c = syntax_->scopeColor(scope);
        html << "<span style=\"color:#" << toHex(c.r) << toHex(c.g) << toHex(c.b) << "\">" << escapeHtml(textBuffer.substr(from, to - from)) << "</span>";
    };
    size_t lineStartPos = lineStart(a);
    while (lineStartPos <= b && lineStartPos <= textBuffer.size()) {
        size_t lineEndPos = lineEnd(lineStartPos);
        size_t from = std::max(lineStartPos, a), to = std::min(lineEndPos, b);
        auto tokens = syntax_->highlightLine(std::string_view(textBuffer.data() + lineStartPos, lineEndPos - lineStartPos), lineStartPos);
        size_t pos = from;
        for (auto& t : tokens) {
            size_t tokenStart = std::max(t.start, from), tokenEnd = std::min(t.start + t.length, to);
            if (tokenEnd <= tokenStart || tokenStart < pos) continue;
            emit(pos, tokenStart, 0);
            emit(tokenStart, tokenEnd, t.scope);
            pos = tokenEnd;
        }
        emit(pos, to, 0);
        if (lineEndPos >= b || lineEndPos >= textBuffer.size()) break;
        html << '\n';
        lineStartPos = lineEndPos + 1;
    }
    html << "</div>";
    return html.str();
}

void Application::copyAsHtml() {
    std::string html = exportToHtml(false);
    SDL_SetClipboardText(html.c_str());
    pluginStatusText_ = "Copied as HTML";
    pluginStatusUntil_ = SDL_GetTicks() + 2500;
}

void Application::printDocument() {
    fs::path file = fs::temp_directory_path() / "moreno_print.html";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "<!doctype html><html><head><meta charset=\"utf-8\"><title>" << escapeHtml(openFile_) << "</title></head><body onload=\"window.print()\">"
            << exportToHtml(true) << "</body></html>";
    }
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", file.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open '" + file.string() + "' &";
    std::system(cmd.c_str());
#else
    std::string cmd = "xdg-open '" + file.string() + "' &";
    std::system(cmd.c_str());
#endif
}
