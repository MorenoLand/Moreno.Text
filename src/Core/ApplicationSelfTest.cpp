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

    std::ofstream out(resultPath, std::ios::binary | std::ios::trunc);
    out << report.str() << (failures ? "FAILED " : "ALL PASSED ") << failures << "\n";
    return failures ? 1 : 0;
}
