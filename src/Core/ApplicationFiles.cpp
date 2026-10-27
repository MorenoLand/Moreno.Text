// Save-time clean-up and detection of files that changed on disk.
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include "Syntax/SyntaxHighlighter.h"
#include "Theme/ThemeEngine.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <filesystem>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

// Applies trim_trailing_white_space_on_save and ensure_newline_at_eof_on_save to the open buffer.
void Application::applySaveTransforms() {
    auto& settings = SettingsManager::instance();
    bool trim = settings.get<bool>("trim_trailing_white_space_on_save", false);
    bool ensureNewline = settings.get<bool>("ensure_newline_at_eof_on_save", false);
    if (!trim && !ensureNewline) return;
    if (binaryView_ || largeFileGuarded_) return;
    std::string out;
    out.reserve(textBuffer.size() + 1);
    std::vector<std::pair<size_t, size_t>> removed;  // position in the old text, bytes removed
    if (trim) {
        size_t pos = 0;
        while (pos <= textBuffer.size()) {
            size_t nl = textBuffer.find('\n', pos);
            size_t end = nl == std::string::npos ? textBuffer.size() : nl;
            size_t keep = end;
            while (keep > pos && (textBuffer[keep - 1] == ' ' || textBuffer[keep - 1] == '\t' || (textBuffer[keep - 1] == '\r' && false))) --keep;
            out.append(textBuffer, pos, keep - pos);
            if (keep < end) removed.emplace_back(keep, end - keep);
            if (nl == std::string::npos) break;
            out.append(textBuffer, end, 1);
            pos = nl + 1;
        }
    } else {
        out = textBuffer;
    }
    if (ensureNewline && !out.empty() && out.back() != '\n') out += '\n';
    if (out == textBuffer) return;
    auto mapPos = [&](size_t p) {
        size_t cut = 0;
        for (auto& [at, len] : removed) {
            if (p >= at + len) cut += len;
            else if (p > at) cut += p - at;
        }
        return p - cut;
    };
    for (auto& s : selections_) { s.anchor = mapPos(s.anchor); s.cursor = mapPos(s.cursor); }
    textBuffer = std::move(out);
    for (auto& s : selections_) { s.anchor = std::min(s.anchor, textBuffer.size()); s.cursor = std::min(s.cursor, textBuffer.size()); }
    invalidateLineIndex();
    syntaxDirty_ = true; indentsDirty_ = true;
}

void Application::noteFileTime(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (!ec) knownFileTimes_[path] = t;
}

// Reloads the open file when it changed on disk and has no unsaved edits; otherwise just says so.
void Application::pollExternalChanges() {
    uint32_t now = SDL_GetTicks();
    if (now - lastExternalPoll_ < 1000) return;
    lastExternalPoll_ = now;
    if (openFilePath_.empty() || binaryView_ || largeFileGuarded_ || openFilePath_.rfind("plugin://", 0) == 0) return;
    std::error_code ec;
    auto t = fs::last_write_time(openFilePath_, ec);
    if (ec) return;
    auto it = knownFileTimes_.find(openFilePath_);
    if (it == knownFileTimes_.end()) { knownFileTimes_[openFilePath_] = t; return; }
    if (it->second == t) return;
    it->second = t;
    std::string name = fs::path(openFilePath_).filename().string();
    if (!dirty_) {
        revertFile();
        pluginStatusText_ = "Reloaded " + name + " (changed on disk)";
    } else {
        pluginStatusText_ = name + " changed on disk, but you have unsaved edits";
    }
    pluginStatusUntil_ = SDL_GetTicks() + 4000;
}

// Re-reads Preferences.sublime-settings after it is saved and applies what can change without a restart.
void Application::applyLiveSettings() {
    auto& settings = SettingsManager::instance();
    settings.load(settings.path());
    settings.ensureDefaults();
    tabSize_ = std::clamp(settings.get<int>("tab_size", tabSize_), 1, 16);
    useTabs_ = !settings.get<bool>("translate_tabs_to_spaces", !useTabs_);
    syntax_->setTabSize(tabSize_);
    syntax_->setUseTabs(useTabs_);
    bool wrap = settings.get<bool>("word_wrap", wordWrap_);
    if (wrap != wordWrap_) { wordWrap_ = wrap; visualLines_.clear(); }
    wrapColumn_ = std::clamp(settings.get<int>("wrap_width", wrapColumn_), 0, 400);
    rulers_ = settings.get<std::vector<int>>("rulers", std::vector<int>());
    autoPair_ = settings.get<bool>("auto_match_enabled", autoPair_);
    scrollPastEnd_ = settings.get<bool>("scroll_past_end", scrollPastEnd_);
    minimapVisible_ = settings.get<bool>("minimap_visible", minimapVisible_);
    autoIndent_ = settings.get<bool>("auto_indent", true);
    matchBrackets_ = settings.get<bool>("match_brackets", true);
    matchSelection_ = settings.get<bool>("match_selection", true);
    int wantedFont = std::clamp(settings.get<int>("font_size", fontSize_), 8, 48);
    if (wantedFont != fontSize_) changeFontSize(wantedFont - fontSize_);
    std::string scheme = settings.get<std::string>("color_scheme", "");
    if (!scheme.empty() && scheme != ThemeEngine::instance().path() && fs::exists(scheme)) applyColorScheme(scheme);
    bool spell = settings.get<bool>("spell_check", spellCheck_);
    if (spell != spellCheck_) { spellCheck_ = spell && ensureSpellDictionary(); }
    visualLines_.clear();
    syntaxDirty_ = true;
    pluginStatusText_ = "Settings applied";
    pluginStatusUntil_ = SDL_GetTicks() + 2500;
}
