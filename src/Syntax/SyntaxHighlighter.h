#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include "../Theme/ThemeEngine.h"

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
    ~SyntaxHighlighter();
    void setLanguage(const std::string& ext);
    void setLanguageByName(const std::string& name);
    void setPluginSyntax(const std::string& syntaxPath, const std::string& colorSchemePath = "");
    void parse(const std::string& text);
    const std::string& languageName() const { return langName_; }
    std::vector<SyntaxToken> highlightLine(std::string_view line, size_t lineOffset) const;
    const SyntaxColor& scopeColor(int scope) const;
    SyntaxColor backgroundColor() const;
    SyntaxColor lineHighlightColor() const;
    SyntaxColor gutterColor() const;
    SyntaxColor minimapBackgroundColor() const;
    int spaceCount() const { return tabSize_; }
    void setTabSize(int sz) { tabSize_ = sz; }
    bool useTabs() const { return useTabs_; }
    void setUseTabs(bool t) { useTabs_ = t; }
    void notifyEdit(size_t startByte, size_t oldEndByte, size_t newEndByte, size_t startRow, size_t startCol, size_t oldEndRow, size_t oldEndCol, size_t newEndRow, size_t newEndCol);
private:
    std::string langName_ = "Plain Text";
    int tabSize_ = 2;
    bool useTabs_ = false;
    enum class PluginSyntaxMode { None, Terminal, Irc, Pm, List, GScript, GOption };
    PluginSyntaxMode pluginSyntaxMode_ = PluginSyntaxMode::None;
    bool useLocalColors_ = false;
    void* parser_ = nullptr;
    void* tree_ = nullptr;
    const void* language_ = nullptr;
    std::shared_ptr<const std::vector<SyntaxToken>> treeTokens_;
    mutable std::mutex tokenMutex_;
    std::thread parseWorker_;
    std::mutex parseMutex_;
    std::condition_variable parseCv_;
    std::string pendingText_;
    const void* pendingLanguage_ = nullptr;
    uint64_t pendingGeneration_ = 0;
    std::atomic<uint64_t> latestGeneration_{0};
    bool parseRequested_ = false;
    bool parseStop_ = false;
    void ensureParseWorker();
    void parseWorkerLoop();
    std::unordered_set<std::string> keywords_;
    std::unordered_set<std::string> builtins_;
    std::unordered_set<std::string> types_;
    // scope indices: 0=plain, 1=keyword, 2=string, 3=comment, 4=number, 5=type, 6=builtin, 7=operator, 8=punctuation
    SyntaxColor colors_[9] = {
        {0.671f, 0.698f, 0.749f}, // plain #abb2bf
        {0.776f, 0.471f, 0.867f}, // keyword #c678dd
        {0.596f, 0.765f, 0.475f}, // string #98c379
        {0.361f, 0.388f, 0.439f}, // comment #5c6370
        {0.820f, 0.604f, 0.400f}, // number #d19a66
        {0.898f, 0.753f, 0.482f}, // type #e5c07b
        {0.380f, 0.686f, 0.937f}, // builtin/function #61afef
        {0.337f, 0.714f, 0.761f}, // operator #56b6c2
        {0.671f, 0.698f, 0.749f}, // punctuation
    };
    SyntaxColor localBackground_ = {0.118f, 0.118f, 0.133f};
    SyntaxColor localLineHighlight_ = {0.165f, 0.165f, 0.188f};
    SyntaxColor localGutter_ = {0.102f, 0.102f, 0.118f};
    SyntaxColor localMinimapBackground_ = {0.082f, 0.082f, 0.094f};
    bool isKeyword(const std::string& w) const { return keywords_.count(w); }
    bool isBuiltin(const std::string& w) const { return builtins_.count(w); }
    bool isType(const std::string& w) const { return types_.count(w); }
    void setupJS();
    void setupPython();
    void setupCPP();
    void setupGenericCode(const std::string& name);
    void setupPlainText();
    void setTreeSitterLanguage(const std::string& name, const void* language);
    std::vector<SyntaxToken> highlightPluginLine(std::string_view line, size_t lineOffset) const;
};
