#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>

struct SyntaxToken {
    size_t start;
    size_t length;
    int scope; // index into color table
};

struct SyntaxColor {
    float r, g, b;
};

class SyntaxHighlighter {
public:
    SyntaxHighlighter();
    void setLanguage(const std::string& ext);
    const std::string& languageName() const { return langName_; }
    std::vector<SyntaxToken> highlightLine(std::string_view line, size_t lineOffset) const;
    const SyntaxColor& scopeColor(int scope) const;
    int spaceCount() const { return tabSize_; }
private:
    std::string langName_ = "Plain Text";
    int tabSize_ = 2;
    std::unordered_set<std::string> keywords_;
    std::unordered_set<std::string> builtins_;
    std::unordered_set<std::string> types_;
    // scope indices: 0=plain, 1=keyword, 2=string, 3=comment, 4=number, 5=type, 6=builtin, 7=operator, 8=punctuation
    SyntaxColor colors_[9] = {
        {0.85f, 0.85f, 0.85f}, // plain
        {0.68f, 0.46f, 0.85f}, // keyword (purple)
        {0.66f, 0.85f, 0.46f}, // string (green)
        {0.50f, 0.55f, 0.50f}, // comment (muted green)
        {0.68f, 0.75f, 0.85f}, // number (light blue)
        {0.46f, 0.68f, 0.85f}, // type (cyan)
        {0.55f, 0.75f, 0.70f}, // builtin (teal)
        {0.85f, 0.65f, 0.45f}, // operator (orange)
        {0.75f, 0.75f, 0.75f}, // punctuation (gray)
    };
    bool isKeyword(const std::string& w) const { return keywords_.count(w); }
    bool isBuiltin(const std::string& w) const { return builtins_.count(w); }
    bool isType(const std::string& w) const { return types_.count(w); }
    void setupJS();
    void setupPython();
    void setupCPP();
    void setupPlainText();
};
