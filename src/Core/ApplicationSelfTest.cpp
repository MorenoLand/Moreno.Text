// `moreno_text.exe --selftest <result file>` runs the editing commands against scratch buffers and
// writes PASS/FAIL lines, so command behavior can be checked without driving the UI.
#include "Core/Application.h"
#include "Syntax/SyntaxHighlighter.h"
#include "Settings/SettingsManager.h"
#include "Commands/KeyBindingManager.h"
#include <chrono>
#include <cstring>
#include <SDL2/SDL.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

int Application::runSelfTest(const std::string& resultPath) {
    std::ostringstream report;
    int failures = 0;
    auto check = [&](const char* name, bool ok, const std::string& detail = std::string()) {
        report << (ok ? "PASS " : "FAIL ") << name;
        if (!ok && !detail.empty()) report << " -> " << detail;
        report << "\n";
        if (!ok) ++failures;
    };
    auto fresh = [&](const std::string& text, size_t cursor = 0) {
        newBuffer();
        textBuffer = text;
        invalidateLineIndex();
        selections_.clear(); selections_.emplace_back(cursor);
        dirty_ = false;
    };
    auto expect = [&](const char* name, const std::string& want) { check(name, textBuffer == want, "got [" + textBuffer + "] want [" + want + "]"); };

    // Deleting lines must leave the line index consistent (the click-after-delete crash).
    fresh("a\n\n\n\nb");
    check("lines before delete", totalLines() == 5);
    textBuffer.erase(2, 3);
    check("stale line index is detected", totalLines() == 2, "totalLines=" + std::to_string(totalLines()));
    check("lineStartForLine past the end is safe", lineStartForLine(4) == textBuffer.size());

    fresh("one\ntwo\nthree", 0);
    selections_[0] = SelRange(0, 13);
    indentSelection();
    expect("indent selection", "  one\n  two\n  three");
    unindentSelection();
    expect("unindent selection", "one\ntwo\nthree");

    fresh("b\nc\na\n");
    sortLines(false);
    expect("sort lines keeps trailing newline", "a\nb\nc\n");
    reverseLines();
    expect("reverse lines", "c\nb\na\n");
    fresh("x\ny\nx\ny\nz");
    uniqueLines();
    expect("unique lines", "x\ny\nz");

    fresh("foo_bar baz", 2);
    convertIdentifierCase(0);
    expect("lowerCamelCase", "fooBar baz");
    convertIdentifierCase(1);
    expect("UpperCamelCase", "FooBar baz");
    convertIdentifierCase(2);
    expect("snake_case", "foo_bar baz");
    convertIdentifierCase(3);
    expect("kebab-case", "foo-bar baz");

    fresh("<div><p>hi</p><br>", 18);
    closeTag();
    expect("close tag skips void and closed tags", "<div><p>hi</p><br></div>");

    fresh("if (a) {\nb();\n}\n");
    reindent();
    expect("reindent", "if (a) {\n  b();\n}\n");

    fresh("abc", 3);
    insertLineAfter();
    expect("insert line after", "abc\n");
    fresh("abc", 0);
    insertLineBefore();
    expect("insert line before", "\nabc");

    fresh("hello world", 11);
    deleteWordBackward();
    expect("delete word backward", "hello ");
    fresh("hello world", 0);
    deleteWordForward();
    expect("delete word forward", " world");
    fresh("hello world", 5);
    deleteToLineEnd();
    expect("delete to end", "hello");
    yank();
    expect("yank restores killed text", "hello world");

    fresh("ab", 1);
    transposeCharacters();
    expect("transpose", "ba");

    // Line commands work on selected blocks and on every caret.
    fresh("a\nb\nc", 2);
    swapLineUp();
    expect("swap line up", "b\na\nc");
    swapLineDown();
    expect("swap line down", "a\nb\nc");
    fresh("a\nb\nc", 4);
    swapLineUp();
    expect("swap last line up keeps its newline", "a\nc\nb");
    fresh("a\nb\nc\nd", 0);
    selections_[0] = SelRange(0, 3);
    swapLineDown();
    expect("swap a two-line block down", "c\na\nb\nd");
    fresh("ab\ncd", 1);
    duplicateLine();
    expect("duplicate line", "ab\nab\ncd");
    fresh("hello", 0);
    selections_[0] = SelRange(1, 3);
    duplicateLine();
    expect("duplicate selection", "helello");
    fresh("a\nb\nc\nd", 0);
    selections_[0] = SelRange(0, 0); selections_.emplace_back(4);
    deleteLine();
    expect("delete line at two carets", "b\nd");
    fresh("a\nb\nc", 4);
    deleteLine();
    expect("delete last line takes the newline before it", "a\nb");
    fresh("a\nb\nc\nd", 0);
    selections_[0] = SelRange(0, 5);
    deleteLine();
    expect("delete every line of a selection", "d");
    fresh("a\n   b\nc", 0);
    joinLines();
    expect("join lines", "a b\nc");
    fresh("a\nb\nc\nd", 0);
    selections_[0] = SelRange(0, 5);
    joinLines();
    expect("join a selection of lines", "a b c\nd");

    autoPair_ = true; autoIndent_ = true;
    fresh("don", 3);
    handleAutoPair("'");
    expect("an apostrophe after a word does not pair", "don'");
    fresh("", 0);
    handleAutoPair("(");
    expect("an opening bracket pairs", "()");
    {
        int savedTab = tabSize_; bool savedTabs = useTabs_;
        tabSize_ = 4; useTabs_ = false;
        fresh("{\n    ", 6);
        handleAutoPair("}");
        expect("a closing bracket dedents its line", "{\n}");
        tabSize_ = savedTab; useTabs_ = savedTabs;
    }
    fresh("a\n  \nb", 4);
    insertNewline();
    expect("Enter on a whitespace-only line empties it", "a\n\n  \nb");

    fresh("<div><p>hi</p></div>", 8);
    expandSelectionToTag();
    check("expand to tag: contents", selections_[0].min() == 8 && selections_[0].max() == 10, std::to_string(selections_[0].min()) + "-" + std::to_string(selections_[0].max()));
    expandSelectionToTag();
    check("expand to tag: whole element", selections_[0].min() == 5 && selections_[0].max() == 14, std::to_string(selections_[0].min()) + "-" + std::to_string(selections_[0].max()));
    expandSelectionToTag();
    check("expand to tag: parent element", selections_[0].min() == 0 && selections_[0].max() == 20);

    fresh("<div\n  a=\"1>\"\n  b=\"2\">x</div>", 8);
    foldedRegions_.clear();
    foldTagAttributes();
    check("fold tag attributes folds the multi-line tag", foldedRegions_.size() == 1 && foldedRegions_.begin()->first == 0 && foldedRegions_.begin()->second == 2);
    foldTagAttributes();
    check("fold tag attributes toggles", foldedRegions_.empty());

    fresh("ab\ncd\nef", 0);
    bookmarks_ = {0, 2};
    selectAllBookmarks();
    check("select all bookmarks", selections_.size() == 2 && selections_[1].min() == 6 && selections_[1].max() == 8);
    bookmarks_.clear();

    fresh("a\nb\nc", 0);
    foldByLevel(1);
    unfoldAll();
    check("fold/unfold do not crash", true);

    fresh("a long line of words that should wrap at a narrow width", 0);
    selections_[0] = SelRange(0, textBuffer.size());
    wrapParagraph(20);
    check("wrap paragraph", textBuffer.find('\n') != std::string::npos && textBuffer.find('\n') <= 20, textBuffer);

    fresh("(a[b]{c})", 4);
    jumpToMatchingBracket();
    check("matching bracket from a closer", selections_[0].cursor == 2, std::to_string(selections_[0].cursor));

    fresh("foo bar foo", 1);
    expandSelectionToWord();
    check("expand to word", selections_[0].min() == 0 && selections_[0].max() == 3);
    expandSelectionToWord();
    check("expand to next occurrence", selections_.size() == 2 && selections_[1].min() == 8);

    fresh("x\r\ny\nz");
    convertLineEndings(false);
    expect("CRLF to LF", "x\ny\nz");
    convertLineEndings(true);
    expect("LF to CRLF", "x\r\ny\r\nz");

    // Snippets: tab stops, defaults, mirrors and moving between stops.
    fresh("");
    insertSnippet("for ${1:i} in ${2:range}: ${1:i}");
    expect("snippet text", "for i in range: i");
    check("snippet selects first stop", selections_.size() >= 1 && selections_[0].min() == 4 && selections_[0].max() == 5);
    textBuffer.replace(4, 1, "idx");
    invalidateLineIndex();
    selections_.clear(); selections_.emplace_back(7);
    advanceSnippet(true);
    expect("snippet mirror updates", "for idx in range: idx");
    check("snippet moves to second stop", selections_[0].min() == 11 && selections_[0].max() == 16, std::to_string(selections_[0].min()) + "," + std::to_string(selections_[0].max()));
    endSnippet();

    fresh("");
    insertSnippet("${1:name} ${1/(.)(.*)/${1:/upcase}$2/} ${1/o/0/g}");
    expect("snippet transform initial", "name Name name");
    textBuffer.replace(0, 4, "foo");
    invalidateLineIndex();
    selections_.clear(); selections_.emplace_back(3);
    advanceSnippet(true);
    expect("snippet transform follows typing", "foo Foo f00");
    endSnippet();

    // Project symbol scan and find in files need a folder; use a temp tree.
    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_tree";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        std::ofstream(dir / "a.py") << "def alpha():\n    pass\nclass Beta:\n    pass\n";
        loadFolder(dir.string());
        find_.query = "alpha"; find_.regex = false; find_.wholeWord = false; find_.caseSensitive = false;
        findPanelIsInFiles_ = true;
        runFindInFiles();
        check("find in files opens results", openFile_ == "Find Results" && textBuffer.find("a.py:") != std::string::npos, textBuffer);
        check("find results are indexed", findHits_.size() == 1 && findHits_[0].second == 1);
        fs::remove_all(dir, ec);
    }

    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_diff";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path file = dir / "d.txt";
        { std::ofstream f(file, std::ios::binary); f << "x\ny\nz\n"; }
        openFile(file.string());
        textBuffer = "x\nY\nz\nw\n";
        invalidateLineIndex(); dirty_ = true;
        refreshDiffHunks(true);
        check("diff finds two hunks", diffHunks_.size() == 2, std::to_string(diffHunks_.size()));
        selections_.clear(); selections_.emplace_back(2);
        revertHunk();
        expect("revert hunk restores the line", "x\ny\nz\nw\n");
        fs::remove_all(dir, ec);
    }

    fresh("a < b");
    selections_[0] = SelRange(0, 5);
    {
        std::string html = exportToHtml(false);
        check("html export escapes text", html.find("&lt;") != std::string::npos && html.find("<div") == 0, html);
    }

    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_spell";
        std::error_code ec;
        fs::create_directories(dir, ec);
        { std::ofstream f(dir / "s.dic", std::ios::binary); f << "2\nhello\nworld\n"; }
        bool hadSpell = spellCheck_;
        spell_.load((dir / "s.dic").string());
        spellCheck_ = true;
        fresh("hello wrld world");
        auto marks = misspelledInLine(0);
        check("spell check flags only the misspelled word", marks.size() == 1 && marks[0].first == 6 && marks[0].second == 10, std::to_string(marks.size()));
        spellCheck_ = hadSpell;
        fs::remove_all(dir, ec);
    }

    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_files";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        std::string created;
        check("create file", createPath(dir.string(), "a.txt", false, created) && fs::exists(dir / "a.txt"));
        check("create refuses an existing name", !createPath(dir.string(), "a.txt", false, created));
        check("create refuses a path separator", !createPath(dir.string(), "x\\y", false, created));
        check("create folder", createPath(dir.string(), "sub", true, created) && fs::is_directory(dir / "sub"));
        check("rename", renamePath((dir / "a.txt").string(), "b.txt") && fs::exists(dir / "b.txt") && !fs::exists(dir / "a.txt"));
        fs::remove_all(dir, ec);
    }

    {
        fresh("a\nb\n");
        size_t first = activeTab_;
        fresh("a\nc\n");
        diffWithTab(first);
        check("diff with tab shows removed and added lines", textBuffer.find("-b") != std::string::npos && textBuffer.find("+c") != std::string::npos, textBuffer);
        fresh("clone me");
        size_t tabCount = tabs_.size();
        cloneTab(activeTab_);
        check("split view clones the tab into a second group", tabs_.size() == tabCount + 1 && editorGroups_.size() == 2 && textBuffer == "clone me");
        collapseEditorGroupsTo(activeTab_);
    }

    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_goto";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir / "src", ec);
        fs::create_directories(dir / "docs", ec);
        { std::ofstream(dir / "src" / "MainWindow.cpp") << "x"; }
        { std::ofstream(dir / "src" / "MainWindow.h") << "x"; }
        { std::ofstream(dir / "src" / "main_helper.py") << "x"; }
        { std::ofstream(dir / "docs" / "readme.txt") << "x"; }
        loadFolder(dir.string());
        goto_.items.clear(); goto_.subtexts.clear(); goto_.data.clear();
        updateFileResults("mw");
        check("fuzzy match ranks MainWindow first", !goto_.items.empty() && goto_.items[0].rfind("MainWindow", 0) == 0, goto_.items.empty() ? std::string("none") : goto_.items[0]);
        goto_.items.clear(); goto_.subtexts.clear(); goto_.data.clear();
        updateFileResults("readme:5");
        check("file:line suffix is parsed", gotoPendingLine_ == 5 && !goto_.items.empty() && goto_.items[0] == "readme.txt");
        goto_.items.clear(); goto_.subtexts.clear(); goto_.data.clear();
        updateFileResults("zzzzqq");
        check("non matching query yields nothing", goto_.items.empty());
        openFile((dir / "src" / "MainWindow.h").string());
        switchHeaderImplementation();
        check("switch header to implementation", openFilePath_.size() >= 14 && openFilePath_.substr(openFilePath_.size() - 14) == "MainWindow.cpp", openFilePath_);
        fs::remove_all(dir, ec);
    }

    {
        fresh("a\na\na");
        selections_.clear();
        selections_.emplace_back(1); selections_.emplace_back(3); selections_.emplace_back(5);
        insertText("X");
        expect("typing inserts at every cursor", "aX\naX\naX");
        check("each cursor lands after its insertion", selections_.size() == 3 && selections_[0].cursor == 2 && selections_[1].cursor == 5 && selections_[2].cursor == 8);
        deleteMulti(false);
        expect("backspace deletes at every cursor", "a\na\na");
        moveAllCursors(SDLK_HOME, false);
        check("home moves every cursor", selections_.size() == 3 && selections_[1].cursor == 2 && selections_[2].cursor == 4);
        fresh("one two one");
        selections_.clear();
        selections_.emplace_back(0, 3); selections_.emplace_back(8, 11);
        insertText("1");
        expect("typing replaces every selection", "1 two 1");
        fresh("x y");
        selections_.clear();
        selections_.emplace_back(0, 1); selections_.emplace_back(2, 3);
        check("copy joins every selection", selectedTextOrLine() == "x\ny");
    }

    {
        fresh("    foo", 7);
        insertNewline();
        expect("enter keeps the indentation", "    foo\n    ");
        fresh("if (x) {", 8);
        insertNewline();
        check("enter after a brace indents one level", textBuffer == "if (x) {\n  " || textBuffer == "if (x) {\n" + std::string("\t"), textBuffer);
        fresh("{}", 1);
        insertNewline();
        check("enter between braces opens a blank line", textBuffer == "{\n  \n}" || textBuffer == "{\n" + std::string("\t") + "\n}", textBuffer);
        check("cursor lands on the blank line", selections_.size() == 1 && selections_[0].cursor == 4 || selections_[0].cursor == 3);
        fresh("a b", 1);
        selections_.clear(); selections_.emplace_back(1); selections_.emplace_back(3);
        insertNewline();
        expect("enter works at every cursor and the second line keeps its indent", "a\n b\n ");
    }

    {
        fresh("ab", 2);
        pressTab(false);
        check("tab inserts spaces up to the next tab stop", textBuffer.size() >= 3 && (useTabs_ ? textBuffer == "ab\t" : textBuffer.size() == 2 + (static_cast<size_t>(tabSize_) - 2 % static_cast<size_t>(tabSize_))), textBuffer);
        fresh("one\ntwo", 0);
        selections_[0] = SelRange(0, 7);
        pressTab(false);
        check("tab with a multi-line selection indents every line", textBuffer.find("\n") != std::string::npos && textBuffer[0] == (useTabs_ ? '\t' : ' ') && textBuffer[textBuffer.find("\n") + 1] == (useTabs_ ? '\t' : ' '), textBuffer);
        pressTab(true);
        expect("shift+tab unindents every line", "one\ntwo");
    }

    {
        fresh("()", 1); handleAutoPair(")");
        check("typing a closing bracket skips over the existing one", textBuffer == "()" && selections_[0].cursor == 2, textBuffer);
        fresh("", 0); handleAutoPair("(");
        expect("typing an opening bracket adds its partner", "()");
        fresh("", 0); insertText("a"); insertText("b"); insertText("c");
        doUndo();
        expect("one undo reverts a quick run of typing", "");
        doRedo();
        expect("redo restores it", "abc");
    }

    {
        auto& st = SettingsManager::instance();
        bool oldTrim = st.get<bool>("trim_trailing_white_space_on_save", false), oldNl = st.get<bool>("ensure_newline_at_eof_on_save", false);
        st.set("trim_trailing_white_space_on_save", true); st.set("ensure_newline_at_eof_on_save", true);
        fresh("a  \nb\t\nc", 4);
        applySaveTransforms();
        expect("save trims trailing whitespace and ends with a newline", "a\nb\nc\n");
        check("cursor follows the trimmed text", selections_[0].cursor == 2, std::to_string(selections_[0].cursor));
        st.set("trim_trailing_white_space_on_save", oldTrim); st.set("ensure_newline_at_eof_on_save", oldNl);
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_reload";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path file = dir / "r.txt";
        { std::ofstream f(file, std::ios::binary); f << "old"; }
        fs::remove(undoStoreFile(file.string()), ec);
        openFile(file.string());
        noteFileTime(file.string());
        lastExternalPoll_ = SDL_GetTicks() - 2000;
        { std::ofstream f(file, std::ios::binary); f << "new content"; }
        fs::last_write_time(file, fs::last_write_time(file) + std::chrono::seconds(5));
        pollExternalChanges();
        expect("a file that changed on disk is reloaded", "new content");
        selections_.clear(); selections_.emplace_back(textBuffer.size());
        insertText("X");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        insertText("Y");
        saveFile();
        expect("typing then saving keeps the text", "new contentXY");
        doUndo();
        expect("undo after save reverts the last step", "new contentX");
        check("undoing past the saved text marks the buffer modified", dirty_);
        doUndo();
        expect("a second undo after save keeps going", "new content");
        doRedo(); doRedo();
        expect("redo after save restores the edits", "new contentXY");
        check("returning to the saved text clears the modified flag", !dirty_);
        {
            saveCurrentTab();
            const TabBuffer& t = tabs_[activeTab_];
            std::string store = (dir / "u.undo").string();
            writeUndoStore(t, store);
            TabBuffer r; r.text = t.text;
            check("undo history survives being written and read back", !t.undoStack.empty() && readUndoStore(r, store) && r.undoStack.size() == t.undoStack.size() && r.redoStack.size() == t.redoStack.size() && *r.undoStack.back().text == *t.undoStack.back().text && r.savedHash == t.savedHash);
            r.text += "!";
            check("history for text that has changed is ignored", !readUndoStore(r, store));
        }
        closeTabNow(activeTab_);
        openFile(file.string());
        doUndo();
        expect("undo history survives closing and reopening the file", "new contentX");
        fs::remove_all(dir, ec);
    }

    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_settings";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path file = dir / "s.sublime-settings";
        { std::ofstream f(file, std::ios::binary); f << "{\n  // comment\n  \"tab_size\": 7,\n  \"rulers\": [60, 90],\n}\n"; }
        auto& st = SettingsManager::instance();
        std::string oldPath = st.path();
        int oldTab = tabSize_;
        std::vector<int> oldRulers = rulers_;
        st.load(file.string());
        check("settings files with comments and trailing commas parse", st.get<int>("tab_size", 0) == 7);
        applyLiveSettings();
        check("live settings apply tab size and rulers", tabSize_ == 7 && rulers_.size() == 2 && rulers_[1] == 90);
        tabSize_ = oldTab; rulers_ = oldRulers;
        st.load(oldPath);
        fs::remove_all(dir, ec);
    }

    {
        fs::path dir = fs::temp_directory_path() / "moreno_selftest_keymap";
        std::error_code ec;
        fs::create_directories(dir, ec);
        { std::ofstream f(dir / "k.sublime-keymap", std::ios::binary);
          f << "[\n  // comment\n  { \"keys\": [\"ctrl+alt+shift+f9\"], \"command\": \"selftest_a\", \"args\": {\"n\": 5} },\n  { \"keys\": [\"ctrl+alt+shift+f9\"], \"command\": \"selftest_b\" },\n  { \"keys\": [\"ctrl+alt+shift+f8\"], \"command\": \"selftest_a\", \"context\": [{\"key\": \"x\"}] },\n  { \"keys\": [\"ctrl+alt+shift+f7\"], \"command\": \"selftest_a\", \"args\": {\"n\": 9} },\n]"; }
        auto& kb = KeyBindingManager::instance();
        int gotArg = 0; std::string last;
        kb.addCommandHandlerWithArgs("selftest_a", [&](const nlohmann::json& a) { gotArg = a.value("n", -1); last = "a"; });
        kb.addCommandHandler("selftest_b", [&] { last = "b"; });
        kb.load((dir / "k.sublime-keymap").string());
        check("keymap with comments dispatches and later bindings win", kb.dispatch("ctrl+alt+shift+f9") && last == "b", last);
        check("keymap args reach the handler", kb.dispatch("ctrl+alt+shift+f7") && gotArg == 9);
        check("bindings with a context are ignored", !kb.dispatch("ctrl+alt+shift+f8"));
        check("key names are normalized", KeyBindingManager::normalize("Ctrl+Shift+Keypad_Minus") == "ctrl+shift+-");
        fs::remove_all(dir, ec);
    }

    {
        bool oldEnabled = pluginEventsEnabled_;
        pluginEventsEnabled_ = true;
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        std::error_code ec;
        fs::create_directories(bridge, ec);
        fresh("hello", 5);
        size_t before = pluginEventSeq_;
        firePluginEvent("on_modified", "selftest.txt", true);
        bool found = false;
        for (auto& e : fs::directory_iterator(bridge, ec)) {
            std::string name = e.path().filename().string();
            if (name.find("-" + std::to_string(before) + "-view_event.request.json") == std::string::npos) continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            f.close();
            found = body.find("\"on_modified\"") != std::string::npos && body.find("hello") != std::string::npos;
            fs::remove(e.path(), ec);
        }
        check("plugin events are written for the host", found);
        pluginEventsEnabled_ = oldEnabled;
    }

    {
        fresh("foo(bar)", 4);
        check("context num_selections", evaluateKeyContext("num_selections", "equal", 1, false));
        check("context preceding_text regex", evaluateKeyContext("preceding_text", "regex_contain", "foo\\($", false));
        check("context following_text regex", evaluateKeyContext("following_text", "regex_match", "bar", false));
        check("context selection_empty", evaluateKeyContext("selection_empty", "equal", true, true));
        check("context not_equal", !evaluateKeyContext("num_selections", "not_equal", 1, false));
        check("selector context matches the language", evaluateKeyContext("selector", "equal", "text.plain", false) || evaluateKeyContext("selector", "equal", "source", false));
        check("selectors: prefix and descendants", scopeSelectorMatches("source.c string", "source.c string.quoted") && scopeSelectorMatches("comment", "source.c comment.line") && !scopeSelectorMatches("comment", "source.c commentary"));
        check("selectors: exclusions and alternatives", !scopeSelectorMatches("source.c - comment", "source.c comment.line") && scopeSelectorMatches("text.html, source.c - comment", "source.c keyword.control"));
        check("unknown context keys fail", !evaluateKeyContext("plugin.thing", "equal", "x", false));
    }

    {
        fresh("h\303\251llo w\303\266rld", 0);
        nlohmann::json edit = {{"key", pluginEventKey(activeTab_)}, {"start", 6}, {"old", "w\303\266rld"}, {"new", "world"}, {"sel", nlohmann::json::array({nlohmann::json::array({11, 11})})}};
        applyPluginViewEdit(edit);
        expect("plugin edit applies by code point offset", "h\303\251llo world");
        check("plugin edit moves the cursor", selections_[0].cursor == 12, std::to_string(selections_[0].cursor));
        edit["old"] = "nope";
        applyPluginViewEdit(edit);
        expect("stale plugin edit is dropped", "h\303\251llo world");
    }

    {
        fresh("h\303\251llo world", 0);
        std::string key = pluginEventKey(activeTab_);
        applyPluginRegions({{"key", key}, {"name", "lint"}, {"regions", nlohmann::json::array({nlohmann::json::array({2, 5})})}, {"scope", "region.redish"}, {"flags", 0}});
        auto it = pluginRegions_.find(key);
        bool ok = it != pluginRegions_.end() && it->second.count("lint") && it->second["lint"].ranges.size() == 1 && it->second["lint"].ranges[0].first == 3 && it->second["lint"].ranges[0].second == 6;
        check("plugin regions are stored as byte ranges", ok);
        applyPluginRegions({{"key", key}, {"name", "lint"}, {"erase", true}});
        check("plugin regions can be erased", pluginRegions_[key].count("lint") == 0);
    }

    {
        fs::path resp = fs::temp_directory_path() / "moreno_selftest_clip.response.json";
        std::error_code ec;
        fs::remove(resp, ec);
        handlePluginRpc("clipboard_set", {{"text", "from-plugin"}});
        handlePluginRpc("clipboard_get", {{"response", resp.string()}});
        std::ifstream f(resp, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        f.close();
        check("plugin clipboard round trip", body.find("from-plugin") != std::string::npos, body);
        fs::remove(resp, ec);
    }

    {
        fresh("fo", 2);
        acPrefix_ = "fo"; acItems_ = {"foobar"}; acInsert_ = {""}; acActive_ = true;
        applyPluginCompletions({{"key", pluginEventKey(activeTab_)}, {"prefix", "fo"}, {"inhibit", false},
                                {"items", nlohmann::json::array({nlohmann::json::array({"format", "format($1)", ""}), nlohmann::json::array({"zzz", "zzz", ""})})}});
        check("plugin completions merge ahead of words", acItems_.size() == 2 && acItems_[0] == "format" && acItems_[1] == "foobar");
        acSelected_ = 0;
        acceptAutocomplete();
        expect("plugin snippet completion replaces the prefix", "format()");
        acActive_ = false;
    }

    {
        fs::path base = fs::temp_directory_path() / "moreno_selftest_multi";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base / "one", ec);
        fs::create_directories(base / "two", ec);
        std::ofstream(base / "one" / "a.txt") << "a";
        std::ofstream(base / "two" / "b.txt") << "b";
        loadFolder((base / "one").string());
        addFolderToProject((base / "two").string());
        check("a second folder joins the project", projectRoots().size() == 2 && sidebarRoots().size() == 2);
        projectFilesAt_ = 0;
        refreshProjectFileList();
        bool hasA = false, hasB = false;
        for (auto& f : projectFiles_) { if (f == "a.txt") hasA = true; if (f.find("b.txt") != std::string::npos) hasB = true; }
        check("Goto Anything lists files from every folder", hasA && hasB);
        removeFolderFromProject((base / "one").string());
        check("removing the first folder promotes the next", projectRoots().size() == 1 && fs::path(sidebarRoot_).filename() == "two");
        closeFolder();
        fs::remove_all(base, ec);
    }

    fresh("");
    insertSnippet("a ${1:x ${2:y}} b");
    expect("nested placeholder text", "a x y b");
    check("outer placeholder selects its whole default", selections_[0].min() == 2 && selections_[0].max() == 5, std::to_string(selections_[0].min()) + "," + std::to_string(selections_[0].max()));
    advanceSnippet(true);
    check("tab moves into the nested placeholder", selections_[0].min() == 4 && selections_[0].max() == 5, std::to_string(selections_[0].min()) + "," + std::to_string(selections_[0].max()));
    endSnippet();
    fresh("");
    insertSnippet("a ${1:x ${2:y}} b$0");
    textBuffer.replace(2, 3, "Q");
    invalidateLineIndex();
    selections_.clear(); selections_.emplace_back(3);
    advanceSnippet(true);
    check("overwriting the outer placeholder drops the nested one", !snippetActive_ && selections_[0].cursor == 5, std::to_string(selections_[0].cursor));
    endSnippet();

    {
        macro_.clear();
        SDL_Event t; std::memset(&t, 0, sizeof(t)); t.type = SDL_TEXTINPUT; std::strcpy(t.text.text, "hi");
        SDL_Event k; std::memset(&k, 0, sizeof(k)); k.type = SDL_KEYDOWN; k.key.keysym.sym = SDLK_LEFT; k.key.keysym.mod = KMOD_LSHIFT;
        macro_ = {t, k};
        auto exported = macroAsSublime();
        check("macro exports as Sublime commands", exported.size() == 2 && exported[0]["command"] == "insert" && exported[0]["args"]["characters"] == "hi" && exported[1]["command"] == "move" && exported[1]["args"]["extend"] == true, exported.dump());
        macro_.clear();
    }

    {   // encodings are worked out when a file is read
        std::string text, name;
        check("UTF-8 is left alone", !decodeOnOpen("caf\xc3\xa9", text, name));
        check("UTF-16 LE with a BOM is decoded", decodeOnOpen(std::string("\xFF\xFE" "h\0i\0", 6), text, name) && text == "hi" && name == "UTF-16 LE", text + "|" + name);
        check("UTF-16 BE with a BOM is decoded", decodeOnOpen(std::string("\xFE\xFF" "\0h\0i", 6), text, name) && text == "hi" && name == "UTF-16 BE", text + "|" + name);
        check("a UTF-8 BOM is dropped and remembered", decodeOnOpen("\xEF\xBB\xBF" "ok", text, name) && text == "ok" && name == "UTF-8 with BOM", text + "|" + name);
#ifdef _WIN32
        check("Latin-1 bytes fall back to Windows 1252", decodeOnOpen("caf\xe9", text, name) && text == "caf\xc3\xa9" && name == "Western (Windows 1252)", text + "|" + name);
#endif
        check("binary data is not decoded", !decodeOnOpen(std::string("ab\0cd\xff", 6), text, name));
    }

    std::ofstream out(resultPath, std::ios::binary | std::ios::trunc);
    out << report.str() << (failures ? "FAILED " : "ALL PASSED ") << failures << "\n";
    return failures ? 1 : 0;
}
