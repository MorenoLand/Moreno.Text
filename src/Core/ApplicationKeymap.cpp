// Maps Sublime's command names (as used in .sublime-keymap files) onto Moreno's commands, and loads keymaps.
#include "Core/Application.h"
#include "Commands/KeyBindingManager.h"
#include "Settings/SettingsManager.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <filesystem>
#include <regex>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

// Evaluates one keymap context condition (the "key", "operator", "operand", "match_all" of a Sublime context entry).
bool Application::evaluateKeyContext(const std::string& key, const std::string& op, const json& operand, bool matchAll) {
    std::vector<json> values;
    auto perSelection = [&](auto fn) { for (auto& s : selections_) values.push_back(fn(s)); };
    size_t size = textBuffer.size();
    if (key == "num_selections") values.push_back(static_cast<int>(selections_.size()));
    else if (key == "selection_empty") perSelection([](const SelRange& s) { return !s.hasSelection(); });
    else if (key == "text") perSelection([&](const SelRange& s) { size_t a = std::min(std::min(s.anchor, s.cursor), size), b = std::min(std::max(s.anchor, s.cursor), size); return textBuffer.substr(a, b - a); });
    else if (key == "preceding_text") perSelection([&](const SelRange& s) { size_t c = std::min(s.cursor, size); return textBuffer.substr(lineStart(c), c - lineStart(c)); });
    else if (key == "following_text") perSelection([&](const SelRange& s) { size_t c = std::min(s.cursor, size); return textBuffer.substr(c, lineEnd(c) - c); });
    else if (key == "overlay_visible") values.push_back(goto_.active);
    else if (key == "panel_visible" || key == "panel_has_focus") values.push_back(find_.active || find_.replaceActive);
    else if (key == "has_next_field" || key == "has_prev_field") values.push_back(snippetActive_);
    else if (key == "read_only") values.push_back(binaryView_ || largeFileGuarded_);
    else if (key == "auto_complete_visible" || key == "popup_visible" || key == "has_snippet") values.push_back(false);
    else if (key.rfind("setting.", 0) == 0) values.push_back(SettingsManager::instance().get<json>(key.substr(8), json(nullptr)));
    else return false;  // scopes (selector) and plugin keys are not available here
    if (values.empty()) return false;
    auto test = [&](const json& v) {
        bool positive = op == "equal" || op == "regex_match" || op == "regex_contain";
        bool result = false;
        if (op == "equal" || op == "not_equal") result = v == operand;
        else if (v.is_string() && operand.is_string()) {
            try {
                std::regex re(operand.get<std::string>());
                std::string s = v.get<std::string>();
                bool contain = op == "regex_contain" || op == "not_regex_contain";
                result = contain ? std::regex_search(s, re) : std::regex_search(s, re, std::regex_constants::match_continuous);
            } catch (...) { return false; }
        } else return false;
        return positive ? result : !result;
    };
    for (auto& v : values) {
        bool ok = test(v);
        if (matchAll && !ok) return false;
        if (!matchAll && ok) return true;
    }
    return matchAll;
}

void Application::loadKeymaps() {
    auto& kb = KeyBindingManager::instance();
    kb.clear();
    kb.setContextEvaluator([this](const std::string& key, const std::string& op, const json& operand, bool matchAll) { return evaluateKeyContext(key, op, operand, matchAll); });
    fs::path defaultDir = fs::path(paths_.dataDir) / "Packages" / "Default";
    kb.load((defaultDir / "Default (Windows).moreno-keymap").string());
    std::error_code ec;
    std::vector<fs::path> packageKeymaps;
    for (const fs::path& root : {fs::path(paths_.packagesDir), fs::path(paths_.cacheDir) / "Installed Packages"}) {
        if (!fs::exists(root, ec)) continue;
        for (fs::directory_iterator pkg(root, ec), end; pkg != end; pkg.increment(ec)) {
            if (ec) break;
            if (!pkg->is_directory(ec) || pkg->path().filename() == "User" || pkg->path().filename() == "Default") continue;
            for (const char* name : {"Default.sublime-keymap", "Default (Windows).sublime-keymap"}) {
                fs::path candidate = pkg->path() / name;
                if (fs::exists(candidate, ec)) packageKeymaps.push_back(candidate);
            }
        }
    }
    std::sort(packageKeymaps.begin(), packageKeymaps.end());
    for (auto& path : packageKeymaps) kb.load(path.string());
    fs::path user = fs::path(paths_.dataDir) / "Packages" / "User";
    for (const char* name : {"Default.sublime-keymap", "Default (Windows).sublime-keymap", "Default (Windows).moreno-keymap"})
        kb.load((user / name).string());
}

void Application::registerCommandHandlers() {
    auto& kb = KeyBindingManager::instance();
    auto plain = [&](const char* name, std::function<void()> fn) { kb.addCommandHandler(name, std::move(fn)); };
    auto withArgs = [&](const char* name, std::function<void(const json&)> fn) { kb.addCommandHandlerWithArgs(name, std::move(fn)); };
    auto flag = [](const json& a, const char* key, bool def) { return a.is_object() && a.contains(key) && a[key].is_boolean() ? a[key].get<bool>() : def; };
    auto integer = [](const json& a, const char* key, int def) { return a.is_object() && a.contains(key) && a[key].is_number_integer() ? a[key].get<int>() : def; };
    auto text = [](const json& a, const char* key, const char* def) { return a.is_object() && a.contains(key) && a[key].is_string() ? a[key].get<std::string>() : std::string(def); };

    plain("new_window", [this] { newWindow(); });
    plain("close_window", [this] { closeWindow(); });
    plain("prompt_open_file", [this] { openFileDialog(); });
    plain("prompt_open_folder", [this] { openFolderDialog(); });
    plain("prompt_save_as", [this] { saveFileAs(); });
    plain("reopen_last_file", [this] { reopenClosedTab(); });
    plain("close", [this] { closeCurrentTab(); });
    plain("close_all", [this] { closeAllFiles(); });
    plain("save_all", [this] { saveAll(); });
    plain("revert", [this] { revertFile(); });
    plain("clone_file", [this] { splitView(); });
    plain("html_print", [this] { printDocument(); });
    plain("switch_file", [this] { switchHeaderImplementation(); });
    plain("toggle_distraction_free", [this] { toggleDistractionFree(); });
    plain("toggle_minimap", [this] { toggleMinimap(); });
    plain("toggle_tabs", [this] { toggleTabs(); });
    plain("toggle_status_bar", [this] { toggleStatusBar(); });
    plain("redo_or_repeat", [this] { commandRedo(); });
    plain("soft_undo", [this] { softUndo(); });
    plain("soft_redo", [this] { softRedo(); });
    plain("paste_from_history", [this] { openPasteHistory(); });
    plain("copy_as_html", [this] { copyAsHtml(); });
    withArgs("select_lines", [=](const json& a) { if (flag(a, "forward", true)) addNextLine(); else addPreviousLine(); });
    withArgs("scroll_lines", [=](const json& a) { float step = static_cast<float>(integer(a, "amount", 1)); scrollY_ = std::max(0.f, scrollY_ - step * 20.f); });
    plain("next_view", [this] { nextTab(); });
    plain("prev_view", [this] { previousTab(); });
    plain("next_view_in_stack", [this] { nextTab(); });
    plain("prev_view_in_stack", [this] { previousTab(); });
    plain("cancel", [this] { singleSelection(); });
    plain("single_selection", [this] { singleSelection(); });
    plain("indent", [this] { indentSelection(); });
    plain("unindent", [this] { unindentSelection(); });
    plain("reindent", [this] { reindent(); });
    withArgs("expand_selection", [=](const json& a) {
        std::string to = text(a, "to", "scope");
        if (to == "line") expandSelectionToLine();
        else if (to == "brackets") expandSelectionToBrackets();
        else if (to == "indentation") expandSelectionToIndentation();
        else expandSelectionToScope();
    });
    plain("expand_selection_to_paragraph", [this] { expandSelectionToIndentation(); });
    plain("find_under_expand_skip", [this] { quickSkipNext(); });
    plain("close_tag", [this] { closeTag(); });
    plain("toggle_record_macro", [this] { toggleMacroRecording(); });
    plain("run_macro", [this] { playMacro(); });
    plain("save_macro", [this] { saveMacro(); });
    withArgs("show_overlay", [=](const json& a) {
        std::string overlay = text(a, "overlay", "goto"), show = text(a, "show", "");
        if (overlay == "command_palette") commandPalette();
        else if (show == "symbol") commandGotoSymbol();
        else if (show == "line") commandGotoLine();
        else if (show == "word") commandGotoWord();
        else if (overlay == "snippets" || overlay == "snippet") openSnippetPicker();
        else commandGotoAnything();
    });
    plain("goto_definition", [this] { gotoDefinition(); });
    plain("goto_reference", [this] { gotoReference(); });
    plain("goto_symbol_in_project", [this] { commandGotoSymbolInProject(); });
    plain("jump_back", [this] { jumpBack(); });
    plain("jump_forward", [this] { jumpForward(); });
    withArgs("show_panel", [=](const json& a) {
        std::string panel = text(a, "panel", "find");
        if (panel == "replace") commandReplace();
        else if (panel == "find_in_files") commandFindInFiles();
        else if (panel == "console") toggleConsole();
        else commandFind();
    });
    plain("replace_next", [this] { replaceNext(); });
    plain("find_next", [this] { findNext(); });
    plain("find_prev", [this] { findPrevious(); });
    plain("find_under", [this] { findUnder(true); });
    plain("find_under_prev", [this] { findUnder(false); });
    plain("find_all_under", [this] { findAllUnder(); });
    plain("slurp_find_string", [this] { useSelectionForFind(); });
    plain("slurp_replace_string", [this] { useSelectionForReplace(); });
    plain("next_result", [this] { nextResult(); });
    plain("prev_result", [this] { previousResult(); });
    plain("next_modification", [this] { nextModification(); });
    plain("prev_modification", [this] { previousModification(); });
    plain("revert_hunk", [this] { revertHunk(); });
    withArgs("toggle_setting", [=](const json& a) {
        std::string setting = text(a, "setting", "");
        if (setting == "spell_check") { toggleSpellCheck(); return; }
        if (setting == "word_wrap") { toggleWordWrap(); return; }
        if (setting.empty()) return;
        auto& st = SettingsManager::instance();
        st.set(setting, !st.get<bool>(setting, false));
        st.save();
        applyLiveSettings();
    });
    plain("next_misspelling", [this] { nextMisspelling(); });
    plain("prev_misspelling", [this] { previousMisspelling(); });
    withArgs("delete_word", [=](const json& a) { if (flag(a, "forward", false)) deleteWordForward(); else deleteWordBackward(); });
    plain("auto_complete", [this] { showCompletions(); });
    plain("build", [this] { runBuild(); });
    plain("cancel_build", [this] { cancelBuild(); });
    plain("transpose", [this] { transposeCharacters(); });
    withArgs("sort_lines", [=](const json& a) { sortLines(flag(a, "case_sensitive", false)); });
    withArgs("permute_lines", [=](const json& a) {
        std::string how = text(a, "operation", "reverse");
        if (how == "unique") uniqueLines(); else if (how == "shuffle") shuffleLines(); else reverseLines();
    });
    withArgs("set_layout", [=](const json& a) {
        size_t columns = 1;
        if (a.is_object() && a.contains("cols") && a["cols"].is_array() && a.contains("rows") && a["rows"].is_array() && a["rows"].size() <= 2) columns = std::max<size_t>(1, a["cols"].size() - 1);
        setLayoutColumns(columns);
    });
    withArgs("focus_group", [=](const json& a) { focusGroup(static_cast<size_t>(std::max(0, integer(a, "group", 0)))); });
    withArgs("move_to_group", [=](const json& a) { moveFileToGroup(static_cast<size_t>(std::max(0, integer(a, "group", 0)))); });
    plain("new_pane", [this] { newGroup(); });
    plain("close_pane", [this] { closeGroup(); });
    withArgs("focus_neighboring_group", [=](const json& a) { focusNeighboringGroup(flag(a, "forward", true)); });
    withArgs("move_to_neighboring_group", [=](const json& a) { moveFileToNeighboringGroup(flag(a, "forward", true)); });
    withArgs("select_by_index", [=](const json& a) { selectTabByIndex(static_cast<size_t>(std::max(0, integer(a, "index", 0)))); });
    plain("select_last_tab", [this] { selectLastTab(); });
    plain("wrap_lines", [this] { wrapParagraph(80); });
    plain("set_mark", [this] { setMark(); });
    plain("select_to_mark", [this] { selectToMark(); });
    plain("delete_to_mark", [this] { deleteToMark(); });
    plain("swap_with_mark", [this] { swapWithMark(); });
    plain("yank", [this] { yank(); });
    plain("show_at_center", [this] { showAtCenter(); });
    plain("increase_font_size", [this] { changeFontSize(1); });
    plain("decrease_font_size", [this] { changeFontSize(-1); });
    plain("reset_font_size", [this] { resetFontSize(); });
    withArgs("insert_snippet", [=](const json& a) {
        std::string contents = text(a, "contents", "");
        if (!contents.empty()) insertSnippet(contents); else openSnippetPicker();
    });
    plain("fold", [this] { foldCurrent(); });
    plain("unfold", [this] { unfoldCurrent(); });
    plain("unfold_all", [this] { commandUnfoldAll(); });
    withArgs("fold_by_level", [=](const json& a) { foldByLevel(integer(a, "level", 1)); });
    withArgs("toggle_comment", [=](const json& a) { if (flag(a, "block", false)) toggleBlockComment(); else toggleLineComment(); });
    plain("title_case", [this] { convertCaseTitle(); });
    plain("swap_case", [this] { convertCaseSwap(); });
    plain("sort_selection", [this] { permuteSelections(0); });
    plain("select_color_scheme", [this] { openColorSchemePicker(); });
    plain("customize_color_scheme", [this] { customizeColorScheme(); });
    plain("new_plugin", [this] { newPlugin(); });
    plain("new_snippet", [this] { newSnippet(); });
    plain("new_syntax", [this] { newSyntax(); });
    plain("new_build_system", [this] { newBuildSystem(); });
    plain("open_project", [this] { openProject(); });
    plain("prompt_select_workspace", [this] { openRecentProjectPicker(); });
    plain("close_workspace", [this] { closeProject(); });
    plain("refresh_folder_list", [this] { refreshFolder(); });
    plain("close_folder_list", [this] { closeFolder(); });
    withArgs("insert", [=](const json& a) { std::string chars = text(a, "characters", ""); if (!chars.empty()) insertText(chars); });
    withArgs("run_command", [=](const json& a) { (void)a; });

    // Anything else that names a plugin command runs that command.
    kb.setFallback([this](const std::string& command, const json&) {
        for (size_t i = 0; i < pluginCommands_.size(); ++i)
            if (pluginCommands_[i].name == command) { executePluginCommand(static_cast<int>(i)); return true; }
        return false;
    });
}
