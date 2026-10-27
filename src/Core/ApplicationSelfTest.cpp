// `moreno_text.exe --selftest <result file>` runs the editing commands against scratch buffers and
// writes PASS/FAIL lines, so command behavior can be checked without driving the UI.
#include "Core/Application.h"
#include "Syntax/SyntaxHighlighter.h"
#include <SDL2/SDL.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

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

    std::ofstream out(resultPath, std::ios::binary | std::ios::trunc);
    out << report.str() << (failures ? "FAILED " : "ALL PASSED ") << failures << "\n";
    return failures ? 1 : 0;
}
