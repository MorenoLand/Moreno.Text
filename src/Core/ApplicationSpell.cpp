// Spell checking with Hunspell dictionaries.
#include "Core/Application.h"
#include "Settings/SettingsManager.h"
#include "Syntax/SyntaxHighlighter.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {

std::string lowered(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isLetter(unsigned char c) { return std::isalpha(c) || c >= 0x80; }

std::vector<std::string> dictionarySearchRoots(const AppPaths& paths) {
    return {paths.packagesDir, (fs::path(paths.cacheDir) / "Installed Packages").string(), paths.installedPackagesDir};
}

}  // namespace

bool Application::ensureSpellDictionary() {
    if (spell_.loaded()) return true;
    std::string path = SettingsManager::instance().get<std::string>("dictionary", "");
    if (path.empty() || !fs::exists(path)) {
        path.clear();
        std::string anyDic;
        for (auto& root : dictionarySearchRoots(paths_)) {
            std::error_code ec;
            if (root.empty() || !fs::exists(root, ec)) continue;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec) || it->path().extension() != ".dic") continue;
                if (it->path().filename() == "en_US.dic") { path = it->path().string(); break; }
                if (anyDic.empty()) anyDic = it->path().string();
            }
            if (!path.empty()) break;
        }
        if (path.empty()) path = anyDic;
    }
    if (path.empty() || !spell_.load(path)) return false;
    for (auto& word : SettingsManager::instance().get<std::vector<std::string>>("added_words", std::vector<std::string>())) spell_.addWord(word);
    return true;
}

void Application::toggleSpellCheck() {
    spellCheck_ = !spellCheck_;
    if (spellCheck_ && !ensureSpellDictionary()) {
        spellCheck_ = false;
        pluginStatusText_ = "No dictionary found: install a package with a Hunspell .dic file (for example Language - English)";
        pluginStatusUntil_ = SDL_GetTicks() + 6000;
        return;
    }
    auto& settings = SettingsManager::instance();
    settings.set("spell_check", spellCheck_);
    settings.save();
    pluginStatusText_ = spellCheck_ ? "Spell check on" : "Spell check off";
    pluginStatusUntil_ = SDL_GetTicks() + 2500;
}

std::vector<std::pair<size_t, size_t>> Application::misspelledInLine(size_t line) const {
    std::vector<std::pair<size_t, size_t>> out;
    if (!spellCheck_ || !spell_.loaded() || line >= totalLines()) return out;
    size_t ls = lineStartForLine(line), le = lineEnd(ls);
    if (le <= ls || le - ls > 2000) return out;
    std::string lang = lowered(syntax_->languageName());
    bool prose = lang.empty() || lang == "plain text" || lang == "markdown" || lang == "text" || lang == "restructuredtext" || lang == "textile" || lang == "latex";
    std::vector<std::pair<size_t, size_t>> allowed;  // byte ranges (relative to the line) worth checking
    if (prose) allowed.push_back({0, le - ls});
    else {
        for (auto& tok : syntax_->highlightLine(std::string_view(textBuffer.data() + ls, le - ls), 0))
            if (tok.scope == 2 || tok.scope == 3) allowed.push_back({tok.start, tok.start + tok.length});
    }
    for (auto& range : allowed) {
        size_t i = range.first, end = std::min(range.second, le - ls);
        while (i < end) {
            if (!isLetter(static_cast<unsigned char>(textBuffer[ls + i]))) { ++i; continue; }
            size_t s = i;
            while (i < end && (isLetter(static_cast<unsigned char>(textBuffer[ls + i])) ||
                               (textBuffer[ls + i] == '\'' && i + 1 < end && isLetter(static_cast<unsigned char>(textBuffer[ls + i + 1])) && i > s))) ++i;
            size_t e = i;
            // Skip identifiers, camelCase, URLs and paths.
            char before = s > 0 ? textBuffer[ls + s - 1] : ' ';
            char after = e < le - ls ? textBuffer[ls + e] : ' ';
            if (before == '_' || after == '_' || std::isdigit(static_cast<unsigned char>(before)) || std::isdigit(static_cast<unsigned char>(after)) ||
                before == '/' || before == '\\' || before == '.' || before == '@' || before == '#' || before == '$' || after == '/' || after == '@') continue;
            std::string word = textBuffer.substr(ls + s, e - s);
            bool mixed = false;
            for (size_t k = 1; k < word.size(); ++k)
                if (std::isupper(static_cast<unsigned char>(word[k])) && std::islower(static_cast<unsigned char>(word[k - 1]))) { mixed = true; break; }
            if (mixed || word.size() < 3) continue;
            if (!spell_.isCorrect(word)) out.emplace_back(s, e);
        }
    }
    return out;
}

void Application::nextMisspelling() {
    if (!spellCheck_ || selections_.empty()) return;
    size_t cursor = selections_[0].max();
    size_t total = totalLines();
    size_t startLine = lineOfPos(cursor);
    for (size_t step = 0; step <= total; ++step) {
        size_t line = (startLine + step) % total;
        size_t ls = lineStartForLine(line);
        for (auto& [s, e] : misspelledInLine(line)) {
            if (step == 0 && ls + s < cursor) continue;
            if (step == 0 && ls + s == cursor && selections_[0].hasSelection()) continue;
            selections_.clear(); selections_.emplace_back(ls + s, ls + e);
            ensureCursorVisible();
            return;
        }
    }
}

void Application::previousMisspelling() {
    if (!spellCheck_ || selections_.empty()) return;
    size_t cursor = selections_[0].min();
    size_t total = totalLines();
    size_t startLine = lineOfPos(cursor);
    for (size_t step = 0; step <= total; ++step) {
        size_t line = (startLine + total - step % total) % total;
        size_t ls = lineStartForLine(line);
        auto marks = misspelledInLine(line);
        for (size_t k = marks.size(); k-- > 0;) {
            if (step == 0 && ls + marks[k].first >= cursor) continue;
            selections_.clear(); selections_.emplace_back(ls + marks[k].first, ls + marks[k].second);
            ensureCursorVisible();
            return;
        }
    }
}

void Application::addWordToDictionary() {
    std::string word = wordOrSelectionAtCursor();
    if (word.empty()) return;
    spell_.addWord(word);
    auto& settings = SettingsManager::instance();
    auto added = settings.get<std::vector<std::string>>("added_words", std::vector<std::string>());
    if (std::find(added.begin(), added.end(), word) == added.end()) added.push_back(word);
    settings.set("added_words", added);
    settings.save();
}

void Application::openSpellSuggestions() {
    if (!spellCheck_ || selections_.empty()) return;
    size_t cursor = selections_[0].cursor;
    size_t s = cursor, e = cursor;
    auto letter = [&](size_t i) { return isLetter(static_cast<unsigned char>(textBuffer[i])) || textBuffer[i] == '\''; };
    while (s > 0 && letter(s - 1)) --s;
    while (e < textBuffer.size() && letter(e)) ++e;
    if (e <= s) return;
    spellWordStart_ = s; spellWordEnd_ = e;
    openGotoWithPrefix("?");
}

void Application::updateSpellSuggestionResults() {
    if (spellWordEnd_ <= spellWordStart_ || spellWordEnd_ > textBuffer.size()) return;
    std::string word = textBuffer.substr(spellWordStart_, spellWordEnd_ - spellWordStart_);
    for (auto& s : spell_.suggest(word, 12)) {
        goto_.items.push_back(s);
        goto_.subtexts.push_back("replace \"" + word + "\"");
        goto_.data.push_back(s);
    }
    goto_.items.push_back("Add \"" + word + "\" to dictionary");
    goto_.subtexts.push_back("");
    goto_.data.push_back("\x01" "add");
}

void Application::applySpellSuggestion(const std::string& choice) {
    if (spellWordEnd_ <= spellWordStart_ || spellWordEnd_ > textBuffer.size()) return;
    if (choice == "\x01" "add") { selections_.clear(); selections_.emplace_back(spellWordStart_, spellWordEnd_); addWordToDictionary(); return; }
    pushUndo();
    textBuffer.replace(spellWordStart_, spellWordEnd_ - spellWordStart_, choice);
    selections_.clear(); selections_.emplace_back(spellWordStart_ + choice.size());
    invalidateLineIndex(); dirty_ = true;
}

void Application::openDictionaryPicker() { openGotoWithPrefix("+"); }

void Application::updateDictionaryResults(const std::string& pattern) {
    std::string needle = lowered(pattern);
    std::error_code ec;
    for (auto& root : dictionarySearchRoots(paths_)) {
        if (root.empty() || !fs::exists(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".dic") continue;
            std::string name = it->path().stem().string();
            if (!needle.empty() && lowered(name).find(needle) == std::string::npos) continue;
            goto_.items.push_back(name + (it->path().string() == spell_.path() ? "  (current)" : ""));
            goto_.subtexts.push_back(it->path().parent_path().filename().string());
            goto_.data.push_back(it->path().string());
        }
    }
}

void Application::chooseDictionary(const std::string& path) {
    auto& settings = SettingsManager::instance();
    settings.set("dictionary", path);
    settings.save();
    spell_ = SpellChecker();
    if (ensureSpellDictionary()) {
        spellCheck_ = true;
        settings.set("spell_check", true);
        settings.save();
        pluginStatusText_ = "Dictionary: " + fs::path(path).stem().string();
    } else {
        pluginStatusText_ = "Couldn't load that dictionary";
    }
    pluginStatusUntil_ = SDL_GetTicks() + 3000;
}

void Application::loadSpellSetting() {
    if (SettingsManager::instance().get<bool>("spell_check", false)) spellCheck_ = ensureSpellDictionary();
}
