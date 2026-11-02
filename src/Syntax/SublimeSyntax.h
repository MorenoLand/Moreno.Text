#pragma once
// A .sublime-syntax interpreter: contexts, push/pop/set, include, with_prototype, meta scopes and capture scopes.
// Embedding another syntax (embed, scope: includes) is resolved through the SyntaxLibrary.
#include "Syntax/OnigRegex.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct ScopeSpan {
    size_t start = 0, length = 0;   // byte offsets in the tokenized text
    std::string scope;              // space separated scope names, outermost first
};

class SublimeSyntax {
public:
    // Parses the text of a .sublime-syntax file. Returns nullptr and fills error when it cannot be used.
    static std::shared_ptr<SublimeSyntax> parse(const std::string& yamlText, std::string& error);

    const std::string& name() const { return name_; }
    const std::string& scope() const { return scope_; }
    const std::vector<std::string>& extensions() const { return extensions_; }
    const std::string& firstLineMatch() const { return firstLine_; }

    // Tokenizes a whole text. Spans cover every byte that has a non-trivial scope; the rest is the base scope.
    std::vector<ScopeSpan> tokenize(const std::string& text, const class SyntaxLibrary* library = nullptr) const;

    struct Impl;
    const Impl* impl() const { return impl_.get(); }
    ~SublimeSyntax();

private:
    SublimeSyntax();
    std::string name_, scope_, firstLine_;
    std::vector<std::string> extensions_;
    std::unique_ptr<Impl> impl_;
    friend class SyntaxLibrary;
};

// All syntaxes the editor knows about, found by scope name or file extension.
class SyntaxLibrary {
public:
    void add(std::shared_ptr<SublimeSyntax> syntax);
    std::shared_ptr<SublimeSyntax> byScope(const std::string& scope) const;
    std::shared_ptr<SublimeSyntax> byExtension(const std::string& ext) const;
    std::shared_ptr<SublimeSyntax> byName(const std::string& name) const;
    const std::vector<std::shared_ptr<SublimeSyntax>>& all() const { return syntaxes_; }
    // Loads every .sublime-syntax under dir (recursively). Returns how many were added.
    size_t loadDirectory(const std::string& dir);
    // Loads the .sublime-syntax files inside a .sublime-package (a zip archive).
    size_t loadPackage(const std::string& packagePath);
    // Loads loose syntaxes under each directory and every .sublime-package found directly inside it.
    size_t loadPaths(const std::vector<std::string>& dirs);
    // Merges syntaxes that use `extends` with their parents. Called after loading.
    void resolveExtends();
    // The library the editor shares. It is filled on a background thread; ready() says when.
    static SyntaxLibrary& shared();
    bool ready() const { return ready_.load(); }
    void setReady() { ready_.store(true); }
    size_t size() const { return syntaxes_.size(); }
private:
    std::vector<std::shared_ptr<SublimeSyntax>> syntaxes_;
    std::atomic<bool> ready_{false};
};
