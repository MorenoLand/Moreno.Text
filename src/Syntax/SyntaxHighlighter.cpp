#include "Syntax/SyntaxHighlighter.h"
#include "Syntax/SublimeSyntax.h"
#include "Theme/ThemeEngine.h"
#include "api.h"
#include <cctype>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <utility>

SyntaxHighlighter::SyntaxHighlighter() { setupPlainText(); }

SyntaxHighlighter::~SyntaxHighlighter() {
    {
        std::lock_guard<std::mutex> lock(parseMutex_);
        parseStop_ = true;
        parseRequested_ = false;
    }
    parseCv_.notify_one();
    if (parseWorker_.joinable()) parseWorker_.join();
    if (tree_) ts_tree_delete((TSTree*)tree_);
    if (parser_) ts_parser_delete((TSParser*)parser_);
}

extern "C" const TSLanguage *tree_sitter_javascript(void);
extern "C" const TSLanguage *tree_sitter_python(void);
extern "C" const TSLanguage *tree_sitter_c(void);
extern "C" const TSLanguage *tree_sitter_cpp(void);
extern "C" const TSLanguage *tree_sitter_markdown(void);

// Maps a scope string (outermost first) to one of the editor's token classes.
int SyntaxHighlighter::classifyScope(const std::string& scope) {
    // innermost scope first; punctuation and meta scopes defer to the scope around them
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < scope.size()) {
        size_t sp = scope.find(' ', pos);
        if (sp == std::string::npos) sp = scope.size();
        if (sp > pos) parts.push_back(scope.substr(pos, sp - pos));
        pos = sp + 1;
    }
    auto starts = [](const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; };
    for (size_t i = parts.size(); i-- > 0;) {
        const std::string& p = parts[i];
        if (starts(p, "comment")) return 3;
        if (starts(p, "string") || starts(p, "constant.character")) return 2;
        if (starts(p, "constant.numeric")) return 4;
        if (starts(p, "constant.language") || starts(p, "constant.other")) return 4;
        if (starts(p, "keyword.operator")) return 7;
        if (starts(p, "keyword") || starts(p, "storage.modifier")) return 1;
        if (starts(p, "storage.type.function")) return 1;
        if (starts(p, "storage")) return 5;
        if (starts(p, "entity.name.function") || starts(p, "support.function") || starts(p, "variable.function") || starts(p, "meta.function-call")) return 6;
        if (starts(p, "entity.name") || starts(p, "support.type") || starts(p, "support.class") || starts(p, "entity.other.inherited-class")) return 5;
        if (starts(p, "variable.language") || starts(p, "support.constant")) return 4;
    }
    return 0;
}

std::string SyntaxHighlighter::scopeNameAt(size_t pos) const {
    std::shared_ptr<const std::vector<ScopeSpan>> spans;
    { std::lock_guard<std::mutex> lock(tokenMutex_); spans = scopeSpans_; }
    if (!sublime_ || !spans || spans->empty()) return {};
    auto it = std::upper_bound(spans->begin(), spans->end(), pos, [](size_t p, const ScopeSpan& s) { return p < s.start; });
    if (it == spans->begin()) return {};
    --it;
    return pos < it->start + it->length ? it->scope : std::string();
}

void SyntaxHighlighter::setSublimeSyntax(std::shared_ptr<SublimeSyntax> syntax) {
    keywords_.clear(); builtins_.clear(); types_.clear();
    { std::lock_guard<std::mutex> lock(tokenMutex_); treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>(); }
    language_ = nullptr;
    pluginSyntaxMode_ = PluginSyntaxMode::None;
    useLocalColors_ = false;
    sublime_ = std::move(syntax);
    langName_ = sublime_ ? sublime_->name() : std::string("Plain Text");
}

void SyntaxHighlighter::setLanguage(const std::string& ext) {
    sublime_.reset();
    keywords_.clear(); builtins_.clear(); types_.clear();
    { std::lock_guard<std::mutex> lock(tokenMutex_); treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>(); }
    language_ = nullptr;
    pluginSyntaxMode_ = PluginSyntaxMode::None;
    useLocalColors_ = false;
    std::string e = ext;
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    static const char* treeSitterExts[] = {"js", "jsx", "mjs", "cjs", "ts", "tsx", "py", "pyw", "c", "cpp", "cc", "cxx", "c++", "hh", "h", "hpp", "hxx", "ipp", "inl", "json", "jsonc", "md", "markdown", nullptr};
    bool treeSitter = false;
    for (int i = 0; treeSitterExts[i]; ++i) if (e == treeSitterExts[i]) treeSitter = true;
    if (!treeSitter && !e.empty() && SyntaxLibrary::shared().ready()) {
        if (auto syntax = SyntaxLibrary::shared().byExtension(e)) { setSublimeSyntax(syntax); return; }
    }
    if (e == "js" || e == "jsx" || e == "mjs" || e == "cjs") { setupJS(); setTreeSitterLanguage("JavaScript", tree_sitter_javascript()); }
    else if (e == "ts" || e == "tsx") { setupJS(); langName_ = "TypeScript"; setTreeSitterLanguage("TypeScript", tree_sitter_javascript()); }
    else if (e == "py" || e == "pyw") { setupPython(); setTreeSitterLanguage("Python", tree_sitter_python()); }
    else if (e == "c") { setupCPP(); setTreeSitterLanguage("C", tree_sitter_c()); }
    else if (e == "cpp" || e == "cc" || e == "cxx" || e == "c++" || e == "hh" || e == "h" || e == "hpp" || e == "hxx" || e == "ipp" || e == "inl") { setupCPP(); setTreeSitterLanguage("C++", tree_sitter_cpp()); }
    else if (e == "json" || e == "jsonc" || e == "sublime-settings" || e == "sublime-keymap" || e == "sublime-menu" || e == "sublime-build" || e == "sublime-project" || e == "sublime-workspace") { setupGenericCode("JSON"); }
    else if (e == "md" || e == "markdown") { setupPlainText(); setTreeSitterLanguage("Markdown", tree_sitter_markdown()); }
    else if (e == "xml" || e == "xsd" || e == "xsl" || e == "svg" || e == "plist") { setupGenericCode("XML"); }
    else if (e == "yml" || e == "yaml") { setupGenericCode("YAML"); }
    else if (e == "toml") { setupGenericCode("TOML"); }
    else if (e == "html" || e == "htm" || e == "xhtml" || e == "shtml") { setupGenericCode("HTML"); }
    else if (e == "css" || e == "scss" || e == "sass" || e == "less") { setupGenericCode("CSS"); }
    else if (e == "cs") { setupGenericCode("C#"); }
    else if (e == "java") { setupGenericCode("Java"); }
    else if (e == "go" || e == "mod" || e == "sum") { setupGenericCode("Go"); }
    else if (e == "rs") { setupGenericCode("Rust"); }
    else if (e == "rb" || e == "rake" || e == "gemspec" || e == "rails") { setupGenericCode("Ruby"); }
    else if (e == "php" || e == "phtml") { setupGenericCode("PHP"); }
    else if (e == "lua") { setupGenericCode("Lua"); }
    else if (e == "sql" || e == "ddl" || e == "dml") { setupGenericCode("SQL"); }
    else if (e == "sh" || e == "bash" || e == "zsh" || e == "fish" || e == "profile" || e == "bashrc" || e == "shellscript") { setupGenericCode("ShellScript"); }
    else if (e == "bat" || e == "cmd") { setupGenericCode("Batch File"); }
    else if (e == "ps1" || e == "psm1" || e == "psd1") { setupGenericCode("PowerShell"); }
    else if (e == "ini" || e == "cfg" || e == "conf" || e == "config" || e == "editorconfig" || e == "gitconfig" || e == "git" || e == "gitattributes" || e == "gitignore") { setupGenericCode("Generic Config"); }
    else if (e == "diff" || e == "patch") { setupGenericCode("Diff"); }
    else if (e == "make" || e == "mk" || e == "mak" || e == "makefile") { setupGenericCode("Makefile"); }
    else if (e == "cmake") { setupGenericCode("CMake"); }
    else if (e == "dockerfile") { setupGenericCode("Dockerfile"); }
    else if (e == "kt" || e == "kts") { setupGenericCode("Kotlin"); }
    else if (e == "swift") { setupGenericCode("Swift"); }
    else if (e == "dart") { setupGenericCode("Dart"); }
    else if (e == "scala" || e == "sc") { setupGenericCode("Scala"); }
    else if (e == "clj" || e == "cljs" || e == "cljc" || e == "edn") { setupGenericCode("Clojure"); }
    else if (e == "erl" || e == "hrl") { setupGenericCode("Erlang"); }
    else if (e == "ex" || e == "exs") { setupGenericCode("Elixir"); }
    else if (e == "hs" || e == "lhs") { setupGenericCode("Haskell"); }
    else if (e == "pl" || e == "pm" || e == "t") { setupGenericCode("Perl"); }
    else if (e == "r") { setupGenericCode("R"); }
    else if (e == "m" || e == "mm") { setupGenericCode("Objective-C"); }
    else if (e == "ml" || e == "mli") { setupGenericCode("OCaml"); }
    else if (e == "pas" || e == "pp" || e == "inc") { setupGenericCode("Pascal"); }
    else if (e == "d") { setupGenericCode("D"); }
    else if (e == "groovy" || e == "gradle") { setupGenericCode("Groovy"); }
    else if (e == "tex" || e == "latex" || e == "ltx") { setupGenericCode("LaTeX"); }
    else if (e == "lisp" || e == "lsp" || e == "el" || e == "scm") { setupGenericCode("Lisp"); }
    else if (e == "matlab") { setupGenericCode("MATLAB"); }
    else if (e == "tcl") { setupGenericCode("TCL"); }
    else if (e == "graphql" || e == "gql") { setupGenericCode("GraphQL"); }
    else if (e == "dot" || e == "gv") { setupGenericCode("Graphviz"); }
    else if (e == "re" || e == "regex" || e == "regexp") { setupGenericCode("Regular Expression"); }
    else if (e == "rst") { setupGenericCode("reStructuredText"); }
    else if (e == "textile") { setupGenericCode("Textile"); }
    else if (e == "as" || e == "as2") { setupGenericCode("ActionScript"); }
    else if (e == "applescript" || e == "scpt") { setupGenericCode("AppleScript"); }
    else if (e == "asp") { setupGenericCode("ASP"); }
    else if (e == "dtd") { setupGenericCode("DTD"); }
    else if (e == "gscript") { setPluginSyntax("Packages/SublimeRC/GScript.sublime-syntax", ""); }
    else if (e == "goption") { setPluginSyntax("Packages/SublimeRC/GOption.sublime-syntax", ""); }
    else setupPlainText();
}

void SyntaxHighlighter::setLanguageByName(const std::string& name) {
    static const struct { const char* name; const char* ext; } map[] = {
        {"JavaScript","js"},{"TypeScript","ts"},{"Python","py"},{"C++","cpp"},{"C","c"},{"HTML","html"},{"CSS","css"},
        {"JSON","json"},{"Java","java"},{"Go","go"},{"Rust","rs"},{"Ruby","rb"},{"PHP","php"},{"PowerShell","ps1"},
        {"SQL","sql"},{"Lua","lua"},{"Markdown","md"},{"XML","xml"},{"YAML","yml"},{"TOML","toml"},{"INI","ini"},
        {"C#","cs"},{"Objective-C","m"},{"Swift","swift"},{"ShellScript","sh"},{"Batch File","bat"},{"Makefile","makefile"},
        {"CMake","cmake"},{"Dockerfile","dockerfile"},{"Kotlin","kt"},{"Dart","dart"},{"Scala","scala"},{"Clojure","clj"},
        {"Erlang","erl"},{"Elixir","ex"},{"Haskell","hs"},{"Perl","pl"},{"R","r"},{"OCaml","ml"},{"Pascal","pas"},
        {"D","d"},{"Groovy","groovy"},{"LaTeX","tex"},{"Lisp","lisp"},{"MATLAB","matlab"},{"TCL","tcl"},{"GraphQL","graphql"},
        {"Graphviz","dot"},{"Diff","diff"},{"Generic Config","ini"},{"Git Config","gitconfig"},{"Git Formats","gitattributes"},
        {"Regular Expression","regex"},{"reStructuredText","rst"},{"Textile","textile"},{"ActionScript","as"},{"ActionScript 2.0","as2"},
        {"AppleScript","applescript"},{"ASP","asp"},{"DTD","dtd"},{"GScript","gscript"},{"GOption","goption"},{"SublimeRC","gscript"},
        {"KSP configuration","kts"},{"Rails","rb"},{"Plain Text",""},
    };
    for (auto& m : map) if (name == m.name) { setLanguage(m.ext); return; }
    if (SyntaxLibrary::shared().ready()) {
        if (auto syntax = SyntaxLibrary::shared().byName(name)) { setSublimeSyntax(syntax); return; }
    }
    sublime_.reset();
    keywords_.clear(); builtins_.clear(); types_.clear();
    { std::lock_guard<std::mutex> lock(tokenMutex_); treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>(); }
    language_ = nullptr;
    pluginSyntaxMode_ = PluginSyntaxMode::None;
    useLocalColors_ = false;
    setupGenericCode(name);
}

static std::string lowerSyntaxString(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static SyntaxColor hexColor(const char* hex) {
    auto nybble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return 0;
    };
    int r = nybble(hex[1]) * 16 + nybble(hex[2]);
    int g = nybble(hex[3]) * 16 + nybble(hex[4]);
    int b = nybble(hex[5]) * 16 + nybble(hex[6]);
    return {r / 255.f, g / 255.f, b / 255.f};
}

static SyntaxColor hexColorString(const std::string& hex) {
    if (hex.size() < 7 || hex[0] != '#') return {0.83f, 0.83f, 0.85f};
    return hexColor(hex.c_str());
}

static std::filesystem::path resolvePackageResource(const std::string& resourcePath) {
    namespace fs = std::filesystem;
    std::string normalized = resourcePath;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.empty()) return {};
    fs::path direct(resourcePath);
    if (direct.is_absolute() && fs::exists(direct)) return direct;
    fs::path rel(normalized);
    fs::path cwd = fs::current_path();
    std::vector<fs::path> candidates;
    if (normalized.rfind("Packages/", 0) == 0) {
        candidates.push_back(cwd / "Data" / rel);
        candidates.push_back(cwd / "bin" / "Release" / "Data" / rel);
        candidates.push_back(cwd.parent_path() / "Data" / rel);
    }
    candidates.push_back(cwd / rel);
    for (auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) return candidate;
    }
    return {};
}

static bool scopeListContains(const std::string& list, const std::string& scope) {
    size_t start = 0;
    while (start < list.size()) {
        size_t comma = list.find(',', start);
        std::string item = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t first = item.find_first_not_of(" \t\r\n");
        size_t last = item.find_last_not_of(" \t\r\n");
        if (first != std::string::npos && item.substr(first, last - first + 1) == scope) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

void loadPluginColorSchemeFile(const std::string& colorSchemePath, SyntaxColor colors[9],
                               SyntaxColor& background, SyntaxColor& lineHighlight,
                               SyntaxColor& gutter, SyntaxColor& minimapBackground) {
    auto path = resolvePackageResource(colorSchemePath);
    if (path.empty()) return;
    try {
        std::string schemeName = lowerSyntaxString(colorSchemePath);
        bool terminalScheme = schemeName.find("terminal.sublime-color-scheme") != std::string::npos;
        bool goptionScheme = schemeName.find("goption") != std::string::npos;
        bool gscriptScheme = schemeName.find("gscript") != std::string::npos;
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        auto data = nlohmann::json::parse(f);
        if (data.contains("globals") && data["globals"].is_object()) {
            auto& g = data["globals"];
            if (g.contains("background")) background = hexColorString(g["background"].get<std::string>());
            if (g.contains("line_highlight")) lineHighlight = hexColorString(g["line_highlight"].get<std::string>());
            if (g.contains("gutter")) gutter = hexColorString(g["gutter"].get<std::string>());
            minimapBackground = background;
            if (g.contains("foreground")) colors[0] = hexColorString(g["foreground"].get<std::string>());
        }
        if (!data.contains("rules") || !data["rules"].is_array()) return;
        for (auto& rule : data["rules"]) {
            if (!rule.contains("scope") || !rule.contains("foreground")) continue;
            std::string scopes = rule["scope"].get<std::string>();
            SyntaxColor color = hexColorString(rule["foreground"].get<std::string>());
            if (terminalScheme) {
                if (scopeListContains(scopes, "keyword")) colors[1] = color;
                if (scopeListContains(scopes, "entity.name.function")) colors[6] = color;
                if (scopeListContains(scopes, "constant.language")) colors[4] = color;
                if (scopeListContains(scopes, "constant.numeric")) colors[5] = color;
                continue;
            }
            if (goptionScheme) {
                if (scopeListContains(scopes, "constant.language.goption") ||
                    scopeListContains(scopes, "keyword.control.goption") ||
                    scopeListContains(scopes, "keyword.operator.goption") ||
                    scopeListContains(scopes, "punctuation.separator.goption")) colors[1] = color;
                if (scopeListContains(scopes, "entity.name.section.goption") ||
                    scopeListContains(scopes, "constant.language.boolean.goption")) colors[5] = color;
                if (scopeListContains(scopes, "keyword.operator.assignment.goption")) colors[7] = color;
                if (scopeListContains(scopes, "variable.parameter.goption")) colors[6] = color;
                if (scopeListContains(scopes, "entity.name.class.goption") ||
                    scopeListContains(scopes, "constant.other.weapon.goption") ||
                    scopeListContains(scopes, "entity.name.filename.goption")) colors[8] = color;
                if (scopeListContains(scopes, "string.unquoted.goption") ||
                    scopeListContains(scopes, "string.other.filepath.goption") ||
                    scopeListContains(scopes, "string.other.list-item.goption") ||
                    scopeListContains(scopes, "string.other.filepath.goption.folder")) colors[2] = color;
                if (scopeListContains(scopes, "string.unquoted.goption.option-value") ||
                    scopeListContains(scopes, "constant.numeric.goption")) colors[4] = color;
                if (scopeListContains(scopes, "comment.line.hash.goption")) colors[3] = color;
                continue;
            }
            if (gscriptScheme) {
                if (scopeListContains(scopes, "keyword.control.gscript") ||
                    scopeListContains(scopes, "storage.type.gscript") ||
                    scopeListContains(scopes, "storage.modifier.gscript") ||
                    scopeListContains(scopes, "keyword.other.gscript")) colors[1] = color;
                if (scopeListContains(scopes, "entity.name.function.gscript") ||
                    scopeListContains(scopes, "variable.language.gscript") ||
                    scopeListContains(scopes, "variable.parameter.gscript")) colors[6] = color;
                if (scopeListContains(scopes, "string.quoted.single.gscript") ||
                    scopeListContains(scopes, "string.quoted.double.gscript")) colors[2] = color;
                if (scopeListContains(scopes, "constant.numeric.gscript") ||
                    scopeListContains(scopes, "constant.language.gscript")) colors[4] = color;
                if (scopeListContains(scopes, "comment.line.double-slash.gscript") ||
                    scopeListContains(scopes, "comment.block.gscript")) colors[3] = color;
                if (scopeListContains(scopes, "keyword.operator.gscript") ||
                    scopeListContains(scopes, "keyword.operator.array.gscript") ||
                    scopeListContains(scopes, "punctuation.gscript")) colors[7] = color;
            }
        }
    } catch (...) {}
}

void SyntaxHighlighter::setPluginSyntax(const std::string& syntaxPath, const std::string& colorSchemePath) {
    sublime_.reset();
    keywords_.clear(); builtins_.clear(); types_.clear();
    { std::lock_guard<std::mutex> lock(tokenMutex_); treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>(); }
    language_ = nullptr;
    pluginSyntaxMode_ = PluginSyntaxMode::None;
    useLocalColors_ = false;
    std::string syntax = lowerSyntaxString(syntaxPath);
    if (syntax.find("terminal.sublime-syntax") != std::string::npos) { pluginSyntaxMode_ = PluginSyntaxMode::Terminal; langName_ = "Terminal"; }
    else if (syntax.find("irc.sublime-syntax") != std::string::npos) { pluginSyntaxMode_ = PluginSyntaxMode::Irc; langName_ = "RC IRC"; }
    else if (syntax.find("pm.sublime-syntax") != std::string::npos) { pluginSyntaxMode_ = PluginSyntaxMode::Pm; langName_ = "RC PM"; }
    else if (syntax.find("list.sublime-syntax") != std::string::npos) { pluginSyntaxMode_ = PluginSyntaxMode::List; langName_ = "RC List"; }
    else if (syntax.find("gscript.sublime-syntax") != std::string::npos) {
        pluginSyntaxMode_ = PluginSyntaxMode::GScript;
        setupGenericCode("GScript");
        static const char* gscriptBuiltins[] = {"triggerclient","triggeraction","sendtonc","echo","temp","player","server","client","this","thiso","level","npc","name","params","obj","isobject","preloadimage",nullptr};
        for (int i = 0; gscriptBuiltins[i]; ++i) builtins_.insert(gscriptBuiltins[i]);
    }
    else if (syntax.find("goption.sublime-syntax") != std::string::npos) {
        pluginSyntaxMode_ = PluginSyntaxMode::GOption;
        setupGenericCode("GOption");
        static const char* goptionBuiltins[] = {"true","false","null","default","option","value",nullptr};
        for (int i = 0; goptionBuiltins[i]; ++i) builtins_.insert(goptionBuiltins[i]);
    }
    else { setupGenericCode("Plugin Syntax"); }
    std::string scheme = lowerSyntaxString(colorSchemePath);
    if (scheme.find("terminal.sublime-color-scheme") != std::string::npos ||
        scheme.find("sublimerc-gscript-editor.sublime-color-scheme") != std::string::npos ||
        scheme.find("sublimerc-goption-editor.sublime-color-scheme") != std::string::npos) {
        useLocalColors_ = true;
        colors_[0] = hexColor("#faf6f3");
        colors_[1] = hexColor("#ff00ff");
        colors_[2] = hexColor("#ffd700");
        colors_[3] = hexColor("#8f98aa");
        colors_[4] = hexColor("#ffb7c5");
        colors_[5] = hexColor("#ffd700");
        colors_[6] = hexColor("#ffb7c5");
        colors_[7] = hexColor("#5fafff");
        colors_[8] = hexColor("#ffb7c5");
        localBackground_ = hexColor("#13141A");
        localLineHighlight_ = hexColor("#171922");
        localGutter_ = hexColor("#13141A");
        localMinimapBackground_ = hexColor("#13141A");
        loadPluginColorSchemeFile(colorSchemePath, colors_, localBackground_, localLineHighlight_, localGutter_, localMinimapBackground_);
    }
}

void SyntaxHighlighter::setTreeSitterLanguage(const std::string& name, const void* language) {
    langName_ = name;
    language_ = language;
}

static int scopeForNodeType(const char* type) {
    if (!type) return 0;
    if (strstr(type, "comment")) return 3;
    if (strstr(type, "string") || strstr(type, "char_literal")) return 2;
    if (strstr(type, "number") || strstr(type, "integer") || strstr(type, "float")) return 4;
    if (strstr(type, "type") || strstr(type, "primitive") || strstr(type, "class_name")) return 5;
    if (strstr(type, "function") || strstr(type, "method")) return 6;
    static const char* keywords[] = {"if","else","for","while","return","class","struct","def","function","const","let","var","import","from","try","catch","switch","case","break","continue","async","await","namespace","template","using","public","private","protected",nullptr};
    for (int i = 0; keywords[i]; ++i) if (strcmp(type, keywords[i]) == 0) return 1;
    static const char* operators[] = {"+","-","*","/","%","=","==","!=","<",">","<=",">=","&&","||","!","&","|","^","~","?",":",nullptr};
    for (int i = 0; operators[i]; ++i) if (strcmp(type, operators[i]) == 0) return 7;
    return 0;
}

static std::vector<SyntaxToken> parseTreeTokens(const std::string& text, const void* language) {
    std::vector<SyntaxToken> tokens;
    if (!language) return tokens;
    TSParser* p = ts_parser_new();
    if (!p) return tokens;
    ts_parser_set_language(p, (const TSLanguage*)language);
    TSTree* newTree = ts_parser_parse_string(p, nullptr, text.data(), (uint32_t)text.size());
    ts_parser_delete(p);
    if (!newTree) return tokens;
    std::vector<TSNode> stack;
    stack.push_back(ts_tree_root_node(newTree));
    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();
        uint32_t childCount = ts_node_child_count(node);
        const char* type = ts_node_type(node);
        int scope = scopeForNodeType(type);
        if (scope && (childCount == 0 || scope == 2 || scope == 3)) {
            uint32_t start = ts_node_start_byte(node), end = ts_node_end_byte(node);
            if (end > start) tokens.push_back({start, end - start, scope});
        }
        for (uint32_t i = 0; i < childCount; ++i) stack.push_back(ts_node_child(node, childCount - 1 - i));
    }
    ts_tree_delete(newTree);
    std::sort(tokens.begin(), tokens.end(), [](const SyntaxToken& a, const SyntaxToken& b) { return a.start < b.start; });
    std::vector<SyntaxToken> nonOverlapping;
    size_t coveredEnd = 0;
    for (auto tok : tokens) {
        if (tok.start < coveredEnd) {
            size_t tokEnd = tok.start + tok.length;
            if (tokEnd <= coveredEnd) continue;
            tok.length = tokEnd - coveredEnd;
            tok.start = coveredEnd;
        }
        coveredEnd = tok.start + tok.length;
        nonOverlapping.push_back(tok);
    }
    return nonOverlapping;
}

// Markdown token classes (see SyntaxHighlighter.h): 9 heading, 10 bold, 11 italic, 12 raw/code, 13 link destination, 14 markers, 15 link text
static void markdownInline(const std::string& t, size_t base, size_t a, size_t b, std::vector<SyntaxToken>& out) {
    auto add = [&](size_t s, size_t e, int cls) { if (e > s) out.push_back({base + s, e - s, cls}); };
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    auto isAlnum = [](char c) { return std::isalnum((unsigned char)c) != 0; };
    size_t i = a;
    while (i < b) {
        char c = t[base + i];
        if (c == '\\' && i + 1 < b) { i += 2; continue; }
        if (c == '`') {
            size_t n = 0; while (i + n < b && t[base + i + n] == '`') ++n;
            size_t j = i + n, close = std::string::npos;
            while (j < b) {
                if (t[base + j] != '`') { ++j; continue; }
                size_t m = 0; while (j + m < b && t[base + j + m] == '`') ++m;
                if (m == n) { close = j + m; break; }
                j += m;
            }
            if (close != std::string::npos) { add(i, close, 12); i = close; } else i += n;
            continue;
        }
        if (c == '*' || c == '_') {
            size_t n = 0; while (i + n < b && t[base + i + n] == c) ++n;
            bool leftOk = i + n < b && !isSpace(t[base + i + n]) && (c == '*' || i == a || !isAlnum(t[base + i - 1]));
            size_t m = std::min<size_t>(n, 3), close = std::string::npos;
            if (leftOk) {
                for (size_t j = i + n; j < b; ++j) {
                    if (t[base + j] == '\\') { ++j; continue; }
                    if (t[base + j] != c) continue;
                    size_t r = 0; while (j + r < b && t[base + j + r] == c) ++r;
                    bool rightOk = j > i + n && !isSpace(t[base + j - 1]) && r >= m && (c == '*' || j + m >= b || !isAlnum(t[base + j + m]));
                    if (rightOk) { close = j + m; break; }
                    j += r - 1;
                }
            }
            if (close != std::string::npos) { add(i, close, m == 1 ? 11 : 10); i = close; } else i += n;
            continue;
        }
        if (c == '~' && i + 1 < b && t[base + i + 1] == '~' && i + 2 < b && !isSpace(t[base + i + 2])) {
            size_t j = t.find("~~", base + i + 2);
            if (j != std::string::npos && j - base < b) { add(i, j - base + 2, 3); i = j - base + 2; continue; }
        }
        if (c == '[' || (c == '!' && i + 1 < b && t[base + i + 1] == '[')) {
            size_t open = c == '!' ? i + 1 : i, depth = 0, j = open;
            for (; j < b; ++j) {
                char d = t[base + j];
                if (d == '\\') { ++j; continue; }
                if (d == '[') ++depth;
                else if (d == ']' && --depth == 0) break;
            }
            if (j < b && j + 1 < b && (t[base + j + 1] == '(' || t[base + j + 1] == '[')) {
                char closer = t[base + j + 1] == '(' ? ')' : ']';
                size_t k = j + 2, pd = 1;
                for (; k < b; ++k) {
                    char d = t[base + k];
                    if (d == '\\') { ++k; continue; }
                    if (d == '(' && closer == ')') ++pd;
                    else if (d == closer && --pd == 0) break;
                }
                if (k < b) { add(i, j + 1, 15); add(j + 1, k + 1, 13); i = k + 1; continue; }
            }
            ++i; continue;
        }
        if (c == '<' && i + 1 < b) {
            size_t j = i + 1;
            while (j < b && t[base + j] != '>' && !isSpace(t[base + j]) && t[base + j] != '<') ++j;
            if (j < b && t[base + j] == '>') {
                std::string inner = t.substr(base + i + 1, j - i - 1);
                bool autolink = inner.find("://") != std::string::npos || (inner.find('@') != std::string::npos && inner.find('.') != std::string::npos);
                add(i, j + 1, autolink ? 13 : 5); i = j + 1; continue;
            }
            // a tag with attributes
            if (std::isalpha((unsigned char)t[base + i + 1]) || t[base + i + 1] == '/' || t[base + i + 1] == '!') {
                size_t k = t.find('>', base + i);
                if (k != std::string::npos && k - base < b) { add(i, k - base + 1, 5); i = k - base + 1; continue; }
            }
            ++i; continue;
        }
        if ((c == 'h' || c == 'H') && (i == a || !isAlnum(t[base + i - 1])) && (t.compare(base + i, 7, "http://") == 0 || t.compare(base + i, 8, "https://") == 0)) {
            size_t j = i; while (j < b && !isSpace(t[base + j]) && t[base + j] != '<' && t[base + j] != '>') ++j;
            while (j > i && std::strchr(".,;:!?)'\"]", t[base + j - 1])) --j;
            add(i, j, 13); i = j; continue;
        }
        ++i;
    }
}

static std::vector<SyntaxToken> markdownTokens(const std::string& t) {
    std::vector<SyntaxToken> out;
    char fenceChar = 0; size_t fenceLen = 0;
    bool prevBlank = true;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t nl = t.find('\n', pos);
        size_t end = nl == std::string::npos ? t.size() : nl;
        size_t le = end; if (le > pos && t[le - 1] == '\r') --le;
        size_t i = pos;
        size_t indent = 0; while (i < le && indent < 4 && (t[i] == ' ')) { ++i; ++indent; }
        auto add = [&](size_t s, size_t e, int cls) { if (e > s) out.push_back({s, e - s, cls}); };
        bool blank = true; for (size_t k = pos; k < le; ++k) if (t[k] != ' ' && t[k] != '\t') { blank = false; break; }
        if (fenceChar) {
            size_t n = 0; while (i + n < le && t[i + n] == fenceChar) ++n;
            bool closing = indent < 4 && n >= fenceLen && t.find_first_not_of(" \t", i + n) >= le;
            add(pos, le, 12);
            if (closing) fenceChar = 0;
        } else if (!blank && indent < 4 && i < le && (t[i] == '`' || t[i] == '~')) {
            char fc = t[i]; size_t n = 0; while (i + n < le && t[i + n] == fc) ++n;
            bool ok = n >= 3 && (fc == '~' || t.find('`', i + n) >= le);
            if (ok) {
                fenceChar = fc; fenceLen = n;
                add(i, i + n, 12);
                size_t info = t.find_first_not_of(" \t", i + n);
                if (info < le) add(info, le, 6);
            } else markdownInline(t, 0, pos, le, out);
        } else if (!blank && indent < 4 && i < le && t[i] == '#') {
            size_t n = 0; while (i + n < le && t[i + n] == '#') ++n;
            if (n <= 6 && (i + n == le || t[i + n] == ' ' || t[i + n] == '\t')) add(pos, le, 9);
            else markdownInline(t, 0, pos, le, out);
        } else if (!blank && indent < 4 && i < le && (t[i] == '-' || t[i] == '*' || t[i] == '_' || t[i] == '=') &&
                   [&] { char hc = t[i]; size_t cnt = 0; for (size_t k = i; k < le; ++k) { if (t[k] == hc) ++cnt; else if (t[k] != ' ' && t[k] != '\t') return false; } return hc == '=' ? cnt >= 1 : cnt >= 3; }()) {
            add(pos, le, 14);
        } else if (!blank) {
            size_t p = i, tail = le;
            if (indent >= 4 && prevBlank) { /* indented code block */ add(pos, le, 12); goto nextLine; }
            while (true) {   // blockquote markers, then an optional list marker
                size_t q = p; while (q < le && t[q] == ' ') ++q;
                if (q < le && t[q] == '>') { add(q, q + 1, 14); p = q + 1; continue; }
                break;
            }
            {
                size_t q = p; while (q < le && (t[q] == ' ' || t[q] == '\t')) ++q;
                size_t mk = q;
                bool listMarker = false;
                if (mk < le && (t[mk] == '-' || t[mk] == '+' || t[mk] == '*') && mk + 1 < le && (t[mk + 1] == ' ' || t[mk + 1] == '\t')) { add(mk, mk + 1, 14); p = mk + 1; listMarker = true; }
                else {
                    size_t d = mk; while (d < le && std::isdigit((unsigned char)t[d]) && d - mk < 9) ++d;
                    if (d > mk && d + 1 < le && (t[d] == '.' || t[d] == ')') && (t[d + 1] == ' ' || t[d + 1] == '\t')) { add(mk, d + 1, 14); p = d + 1; listMarker = true; }
                }
                if (listMarker) {   // task list box
                    size_t s = p; while (s < le && t[s] == ' ') ++s;
                    if (s + 2 < le && t[s] == '[' && (t[s + 1] == ' ' || t[s + 1] == 'x' || t[s + 1] == 'X') && t[s + 2] == ']' && (s + 3 >= le || t[s + 3] == ' ')) { add(s, s + 3, 14); p = s + 3; }
                }
            }
            // link reference definition:  [label]: destination
            {
                size_t s = p; while (s < le && t[s] == ' ') ++s;
                if (s < le && t[s] == '[' && s == i) {
                    size_t cb = t.find("]:", s);
                    if (cb != std::string::npos && cb < le) {
                        size_t ds = t.find_first_not_of(" \t", cb + 2);
                        if (ds < le) {
                            size_t de = ds; while (de < le && t[de] != ' ' && t[de] != '\t') ++de;
                            add(s, cb + 2, 15); add(ds, de, 13);
                            p = de;
                        }
                    }
                }
            }
            if (t.find('|', p) < tail && t.find_first_not_of(" \t|:-", p) >= tail && t.find('-', p) < tail) add(p, tail, 14);   // table delimiter row
            else {
                size_t before = out.size();
                markdownInline(t, 0, p, tail, out);
                if (t.find_first_not_of(" \t", pos) != std::string::npos && t[t.find_first_not_of(" \t", pos)] == '|') {
                    // table row: pipes as markers, inline spans stay as they are
                    std::vector<SyntaxToken> pipes;
                    for (size_t k = p; k < tail; ++k) if (t[k] == '|') pipes.push_back({k, 1, 14});
                    for (auto& pt : pipes) {
                        bool inside = false;
                        for (size_t q = before; q < out.size(); ++q) if (pt.start >= out[q].start && pt.start < out[q].start + out[q].length) { inside = true; break; }
                        if (!inside) out.push_back(pt);
                    }
                    std::sort(out.begin() + (long)before, out.end(), [](const SyntaxToken& x, const SyntaxToken& y) { return x.start < y.start; });
                }
            }
        }
        nextLine:
        prevBlank = blank;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

void SyntaxHighlighter::ensureParseWorker() {
    if (!parseWorker_.joinable()) parseWorker_ = std::thread(&SyntaxHighlighter::parseWorkerLoop, this);
}

void SyntaxHighlighter::parseWorkerLoop() {
    for (;;) {
        std::string text;
        const void* language = nullptr;
        std::shared_ptr<SublimeSyntax> sublime;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(parseMutex_);
            parseCv_.wait(lock, [&] { return parseStop_ || parseRequested_; });
            if (parseStop_) return;
            text = std::move(pendingText_);
            language = pendingLanguage_;
            sublime = pendingSublime_;
            generation = pendingGeneration_;
            parseRequested_ = false;
        }
        std::vector<SyntaxToken> tokens;
        std::shared_ptr<const std::vector<ScopeSpan>> spansOut;
        if (sublime) {
            auto allSpans = std::make_shared<std::vector<ScopeSpan>>(sublime->tokenize(text, &SyntaxLibrary::shared()));
            if (text.size() < 4u * 1024u * 1024u) spansOut = allSpans;
            for (const ScopeSpan& span : *allSpans) {
                int cls = classifyScope(span.scope);
                if (cls == 0) continue;
                if (!tokens.empty() && tokens.back().scope == cls && tokens.back().start + tokens.back().length == span.start) tokens.back().length += span.length;
                else tokens.push_back({span.start, span.length, cls});
            }
        } else if (language && language == (const void*)tree_sitter_markdown()) tokens = markdownTokens(text);
        else tokens = parseTreeTokens(text, language);
        auto parsed = std::make_shared<const std::vector<SyntaxToken>>(std::move(tokens));
        if (generation == latestGeneration_.load()) {
            std::lock_guard<std::mutex> lock(tokenMutex_);
            treeTokens_ = std::move(parsed);
            scopeSpans_ = std::move(spansOut);
        }
    }
}

void SyntaxHighlighter::parse(const std::string& text) {
    if (!language_ && !sublime_) {
        std::lock_guard<std::mutex> lock(tokenMutex_);
        treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>();
        return;
    }
    ensureParseWorker();
    {
        std::lock_guard<std::mutex> lock(parseMutex_);
        pendingText_ = text;
        pendingLanguage_ = language_;
        pendingSublime_ = sublime_;
        pendingGeneration_++;
        latestGeneration_.store(pendingGeneration_);
        parseRequested_ = true;
    }
    parseCv_.notify_one();
}

void SyntaxHighlighter::setupJS() {
    langName_ = "JavaScript";
    static const char* kw[] = {"break","case","catch","class","const","continue","debugger","default","delete","do","else","export","extends","finally","for","function","if","import","in","instanceof","let","new","return","static","super","switch","this","throw","try","typeof","var","void","while","with","yield","async","await","from","of",nullptr};
    static const char* bi[] = {"console","document","window","Math","JSON","Object","Array","String","Number","Boolean","Date","RegExp","Error","Map","Set","Promise","Symbol","null","undefined","true","false","NaN","Infinity","parseInt","parseFloat","isNaN","require","module","exports","process",nullptr};
    static const char* tp[] = {"string","number","boolean","any","void","never","null","undefined",nullptr};
    for (int i = 0; kw[i]; ++i) keywords_.insert(kw[i]);
    for (int i = 0; bi[i]; ++i) builtins_.insert(bi[i]);
    for (int i = 0; tp[i]; ++i) types_.insert(tp[i]);
}
void SyntaxHighlighter::setupPython() {
    langName_ = "Python";
    static const char* kw[] = {"False","None","True","and","as","assert","async","await","break","class","continue","def","del","elif","else","except","finally","for","from","global","if","import","in","is","lambda","nonlocal","not","or","pass","raise","return","try","while","with","yield",nullptr};
    static const char* bi[] = {"print","range","len","int","str","float","list","dict","set","tuple","type","isinstance","hasattr","getattr","setattr","input","open","super","property","staticmethod","classmethod","enumerate","zip","map","filter","sorted","reversed","any","all","min","max","sum","abs","round","hex","oct","bin","chr","ord","id","hash","callable","iter","next","format","repr","ascii","dir","vars","help","breakpoint",nullptr};
    static const char* tp[] = {"int","float","str","bool","bytes","bytearray","list","tuple","dict","set","frozenset","complex","memoryview","range","type","NoneType","object",nullptr};
    for (int i = 0; kw[i]; ++i) keywords_.insert(kw[i]);
    for (int i = 0; bi[i]; ++i) builtins_.insert(bi[i]);
    for (int i = 0; tp[i]; ++i) types_.insert(tp[i]);
}
void SyntaxHighlighter::setupCPP() {
    langName_ = "C++";
    static const char* kw[] = {"alignas","alignof","and","and_eq","asm","auto","bitand","bitor","bool","break","case","catch","char","char8_t","char16_t","char32_t","class","compl","concept","const","consteval","constexpr","constinit","const_cast","continue","co_await","co_return","co_yield","decltype","default","delete","do","double","dynamic_cast","else","enum","explicit","export","extern","false","float","for","friend","goto","if","inline","long","mutable","namespace","new","noexcept","not","not_eq","nullptr","operator","or","or_eq","private","protected","public","register","reinterpret_cast","requires","return","short","signed","sizeof","static","static_assert","static_cast","struct","switch","template","this","thread_local","throw","true","try","typedef","typeid","typename","union","unsigned","using","virtual","void","volatile","wchar_t","while","xor","xor_eq","override","final",nullptr};
    static const char* bi[] = {"size_t","uint8_t","uint16_t","uint32_t","uint64_t","int8_t","int16_t","int32_t","int64_t","ptrdiff_t","intptr_t","uintptr_t","nullptr_t","NULL","stdin","stdout","stderr","printf","scanf","malloc","free","calloc","realloc","memcpy","memset","memmove","strcmp","strlen","strcpy","strcat","atoi","atof","strtol","strtod","abs","min","max","swap","sort","find","begin","end","size","empty","front","back","push_back","push_front","pop_back","pop_front","insert","erase","emplace","emplace_back","clear","reserve","resize","count","find","map","set","vector","string","array","deque","list","forward_list","queue","stack","priority_queue","unordered_map","unordered_set","pair","tuple","optional","variant","any","unique_ptr","shared_ptr","weak_ptr","make_unique","make_shared","make_pair","move","forward","static_pointer_cast","dynamic_pointer_cast",nullptr};
    static const char* tp[] = {"int","long","short","float","double","char","bool","void","unsigned","signed","wchar_t","char16_t","char32_t","char8_t","auto","decltype","size_t","ptrdiff_t","int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t","uint64_t",nullptr};
    for (int i = 0; kw[i]; ++i) keywords_.insert(kw[i]);
    for (int i = 0; bi[i]; ++i) builtins_.insert(bi[i]);
    for (int i = 0; tp[i]; ++i) types_.insert(tp[i]);
}
void SyntaxHighlighter::setupGenericCode(const std::string& name) {
    langName_ = name;
    static const char* kw[] = {"and","as","async","await","break","case","catch","class","const","continue","def","default","do","else","enum","export","extends","false","for","from","func","function","if","import","in","interface","let","module","namespace","new","nil","none","not","null","or","package","private","protected","public","return","static","struct","switch","this","throw","true","try","type","using","var","while","with",nullptr};
    static const char* tp[] = {"bool","boolean","byte","char","double","float","int","integer","long","number","object","short","string","void",nullptr};
    static const char* bi[] = {"print","printf","echo","console","log","len","length","size","map","set","list","dict","array",nullptr};
    for (int i = 0; kw[i]; ++i) keywords_.insert(kw[i]);
    for (int i = 0; tp[i]; ++i) types_.insert(tp[i]);
    for (int i = 0; bi[i]; ++i) builtins_.insert(bi[i]);
}
void SyntaxHighlighter::setupPlainText() { langName_ = "Plain Text"; }

static bool isWordChar(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::vector<SyntaxToken> SyntaxHighlighter::highlightLine(std::string_view line, size_t lineOffset) const {
    if (pluginSyntaxMode_ == PluginSyntaxMode::Terminal ||
        pluginSyntaxMode_ == PluginSyntaxMode::Irc ||
        pluginSyntaxMode_ == PluginSyntaxMode::Pm ||
        pluginSyntaxMode_ == PluginSyntaxMode::List ||
        pluginSyntaxMode_ == PluginSyntaxMode::GOption) {
        return highlightPluginLine(line, lineOffset);
    }
    std::shared_ptr<const std::vector<SyntaxToken>> treeSnapshot;
    {
        std::lock_guard<std::mutex> lock(tokenMutex_);
        treeSnapshot = treeTokens_;
    }
    if (treeSnapshot && !treeSnapshot->empty()) {
        std::vector<SyntaxToken> out;
        size_t lineEnd = lineOffset + line.size();
        auto it = std::lower_bound(treeSnapshot->begin(), treeSnapshot->end(), lineOffset, [](const SyntaxToken& tok, size_t pos) { return tok.start + tok.length <= pos; });
        for (; it != treeSnapshot->end() && it->start < lineEnd; ++it) {
            size_t s = std::max(it->start, lineOffset), e = std::min(it->start + it->length, lineEnd);
            if (!out.empty()) {
                size_t prevEnd = out.back().start + out.back().length;
                if (s < prevEnd) s = prevEnd;
            }
            if (e > s) out.push_back({s, e - s, it->scope});
        }
        return out;
    }
    std::vector<SyntaxToken> tokens;
    size_t i = 0, len = line.size();
    while (i < len) {
        char c = line[i];
        // whitespace
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        // single-line comment //
        if (c == '/' && i + 1 < len && line[i+1] == '/') {
            tokens.push_back({lineOffset + i, len - i, 3});
            break;
        }
        // single-line comment #
        if (c == '#') {
            tokens.push_back({lineOffset + i, len - i, 3});
            break;
        }
        // string " or '
        if (c == '"' || c == '\'') {
            char q = c; size_t start = i; ++i;
            while (i < len && line[i] != q) { if (line[i] == '\\' && i + 1 < len) ++i; ++i; }
            if (i < len) ++i;
            tokens.push_back({lineOffset + start, i - start, 2});
            continue;
        }
        // template literal `
        if (c == '`') {
            size_t start = i; ++i;
            while (i < len && line[i] != '`') { if (line[i] == '\\' && i + 1 < len) ++i; ++i; }
            if (i < len) ++i;
            tokens.push_back({lineOffset + start, i - start, 2});
            continue;
        }
        // number
        if (isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            while (i < len && (isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.' || line[i] == 'x' || line[i] == 'X' || line[i] == 'e' || line[i] == 'E' || line[i] == 'b' || line[i] == 'B' || ((line[i] == 'l' || line[i] == 'L' || line[i] == 'u' || line[i] == 'U' || line[i] == 'f' || line[i] == 'F') && i > start))) ++i;
            tokens.push_back({lineOffset + start, i - start, 4});
            continue;
        }
        // word (keyword/builtin/type/identifier)
        if (isWordChar(c)) {
            size_t start = i;
            while (i < len && isWordChar(line[i])) ++i;
            std::string word(line.substr(start, i - start));
            int scope = 0;
            if (isKeyword(word)) scope = 1;
            else if (isType(word)) scope = 5;
            else if (isBuiltin(word)) scope = 6;
            tokens.push_back({lineOffset + start, i - start, scope});
            continue;
        }
        // operator
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '=' || c == '!' || c == '<' || c == '>' || c == '&' || c == '|' || c == '^' || c == '~') {
            size_t start = i; ++i;
            if (i < len && (line[i] == '=' || line[i] == c)) ++i; // ==, ++, --, &&, ||, etc.
            tokens.push_back({lineOffset + start, i - start, 7});
            continue;
        }
        // punctuation
        if (c == '(' || c == ')' || c == '{' || c == '}' || c == '[' || c == ']' || c == ';' || c == ',' || c == ':' || c == '.' || c == '?') {
            tokens.push_back({lineOffset + i, 1, 8});
            ++i; continue;
        }
        ++i; // skip unknown
    }
    return tokens;
}

int SyntaxHighlighter::scopeStyle(int scope) const {
    if (useLocalColors_ || scope < 1 || scope >= 16) return 0;
    static const char* scopeNames[] = {"", "keyword", "string", "comment", "number", "type", "function", "operator", "punctuation",
        "markup.heading", "markup.bold", "markup.italic", "markup.raw", "markup.underline.link", "punctuation.definition.markup", "string.other.link.title"};
    return ThemeEngine::instance().styleForScope(scopeNames[scope]);
}

const SyntaxColor& SyntaxHighlighter::scopeColor(int scope) const {
    if (useLocalColors_) {
        if (scope < 0 || scope >= 16) scope = 0;
        return colors_[scope];
    }
    static const char* scopeNames[] = {"","keyword","string","comment","number","type","function","operator","punctuation",
        "markup.heading", "markup.bold", "markup.italic", "markup.raw", "markup.underline.link", "punctuation.definition.markup", "string.other.link.title"};
    static thread_local SyntaxColor cached[16];
    if (scope >= 1 && scope < 16) {
        ThemeColor tc = ThemeEngine::instance().colorForScope(scopeNames[scope]);
        cached[scope] = {tc.r, tc.g, tc.b}; return cached[scope];
    }
    ThemeColor tc = ThemeEngine::instance().fgColor();
    cached[0] = {tc.r, tc.g, tc.b}; return cached[0];
}

SyntaxColor SyntaxHighlighter::backgroundColor() const {
    if (useLocalColors_) return localBackground_;
    ThemeColor c = ThemeEngine::instance().bgColor();
    return {c.r, c.g, c.b};
}

SyntaxColor SyntaxHighlighter::lineHighlightColor() const {
    if (useLocalColors_) return localLineHighlight_;
    ThemeColor c = ThemeEngine::instance().lineHighlightColor();
    return {c.r, c.g, c.b};
}

SyntaxColor SyntaxHighlighter::gutterColor() const {
    if (useLocalColors_) return localGutter_;
    ThemeColor c = ThemeEngine::instance().gutterColor();
    return {c.r, c.g, c.b};
}

SyntaxColor SyntaxHighlighter::minimapBackgroundColor() const {
    if (useLocalColors_) return localMinimapBackground_;
    ThemeColor c = ThemeEngine::instance().minimapBg();
    return {c.r, c.g, c.b};
}

std::vector<SyntaxToken> SyntaxHighlighter::highlightPluginLine(std::string_view line, size_t lineOffset) const {
    std::vector<SyntaxToken> tokens;
    auto add = [&](size_t start, size_t len, int scope) {
        if (start < line.size() && len > 0) tokens.push_back({lineOffset + start, std::min(len, line.size() - start), scope});
    };
    auto finish = [&]() {
        std::sort(tokens.begin(), tokens.end(), [](const SyntaxToken& a, const SyntaxToken& b) {
            if (a.start != b.start) return a.start < b.start;
            return a.length > b.length;
        });
        std::vector<SyntaxToken> out;
        size_t coveredEnd = lineOffset;
        for (auto tok : tokens) {
            size_t tokEnd = tok.start + tok.length;
            if (tokEnd <= coveredEnd) continue;
            if (tok.start < coveredEnd) {
                tok.length = tokEnd - coveredEnd;
                tok.start = coveredEnd;
            }
            coveredEnd = tok.start + tok.length;
            out.push_back(tok);
        }
        return out;
    };
    if (pluginSyntaxMode_ == PluginSyntaxMode::GOption) {
        size_t len = line.size();
        size_t p = 0;
        while (p < len && std::isspace((unsigned char)line[p])) ++p;
        if (p < len && line[p] == '#') {
            add(p, len - p, 3);
            return finish();
        }
        if (p < len && line[p] == '[') {
            size_t close = line.find(']', p + 1);
            if (close != std::string_view::npos) {
                add(p, 1, 1);
                size_t nameStart = p + 1;
                while (nameStart < close && std::isspace((unsigned char)line[nameStart])) ++nameStart;
                size_t nameEnd = close;
                while (nameEnd > nameStart && std::isspace((unsigned char)line[nameEnd - 1])) --nameEnd;
                add(nameStart, nameEnd - nameStart, 5);
                add(close, 1, 1);
                return finish();
            }
        }
        auto addInlineBracketsAndCommas = [&]() {
            size_t pos = 0;
            while ((pos = line.find('[', pos)) != std::string_view::npos) {
                size_t close = line.find(']', pos + 1);
                if (close == std::string_view::npos) break;
                add(pos, 1, 1);
                add(pos + 1, close - pos - 1, 1);
                add(close, 1, 1);
                pos = close + 1;
            }
            pos = 0;
            while ((pos = line.find(',', pos)) != std::string_view::npos) add(pos++, 1, 1);
        };
        size_t eq = line.find('=');
        if (eq != std::string_view::npos && eq > p) {
            size_t keyEnd = eq;
            while (keyEnd > p && std::isspace((unsigned char)line[keyEnd - 1])) --keyEnd;
            add(p, keyEnd - p, 6);
            add(eq, 1, 7);
            size_t valueStart = eq + 1;
            if (valueStart < len) add(valueStart, len - valueStart, 4);
            addInlineBracketsAndCommas();
            return finish();
        }
        size_t colon = line.find(':');
        if (colon != std::string_view::npos && colon > p && line.find('[') == std::string_view::npos) {
            add(p, colon - p, 6);
            add(colon, 1, 1);
            size_t valueStart = colon + 1;
            while (valueStart < len && std::isspace((unsigned char)line[valueStart])) ++valueStart;
            if (valueStart < len) {
                std::string value(line.substr(valueStart));
                std::string lowerValue = lowerSyntaxString(value);
                add(valueStart, len - valueStart, lowerValue == "true" || lowerValue == "false" ? 5 : 2);
            }
            return finish();
        }
        static const char* fileWords[] = {"file", "level", "body", "shield", "sword", "head", "gmap", nullptr};
        for (int i = 0; fileWords[i]; ++i) {
            size_t wordLen = std::strlen(fileWords[i]);
            if (len >= p + wordLen && line.substr(p, wordLen) == fileWords[i] && p + wordLen < len && std::isspace((unsigned char)line[p + wordLen])) {
                add(p, wordLen, 6);
                size_t valueStart = p + wordLen + 1;
                while (valueStart < len && std::isspace((unsigned char)line[valueStart])) ++valueStart;
                add(valueStart, len - valueStart, 2);
                return finish();
            }
        }
        if (len >= p + 2 && (line.substr(p, 2) == "r " || line.substr(p, 3) == "rw ")) {
            size_t rightsLen = line.substr(p, 3) == "rw " ? 2 : 1;
            add(p, rightsLen, 8);
            size_t folderStart = p + rightsLen + 1;
            add(folderStart, len - folderStart, 2);
            return finish();
        }
        if (p < len && (line[p] == '-' || line[p] == '*')) {
            add(p, 1, 1);
            if (p + 1 < len) add(p + 1, len - p - 1, 8);
            return finish();
        }
        size_t pos = p;
        while (pos < len) {
            if (std::isdigit((unsigned char)line[pos])) {
                size_t start = pos;
                while (pos < len && std::isdigit((unsigned char)line[pos])) ++pos;
                add(start, pos - start, 4);
                continue;
            }
            ++pos;
        }
        addInlineBracketsAndCommas();
        return finish();
    }
    if (line.rfind("> ", 0) == 0) {
        add(0, 2, 4);
        return finish();
    }
    if (!line.empty() && line[0] == '[') {
        size_t close = line.find(']');
        if (close != std::string_view::npos) {
            add(0, close + 1, 4);
            size_t textStart = close + 1;
            while (textStart < line.size() && line[textStart] == ' ') ++textStart;
            size_t colon = line.find(':', textStart);
            if (colon != std::string_view::npos && colon > textStart && colon - textStart < 64) {
                add(textStart, colon - textStart + 1, 6);
            } else {
                size_t paren = line.find('(', textStart);
                if (paren != std::string_view::npos && paren > textStart && paren - textStart < 64) add(textStart, paren - textStart, 6);
            }
        }
    }
    std::string lower(line);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto isBoundary = [&](size_t pos) {
        return pos >= lower.size() || !std::isalnum((unsigned char)lower[pos]);
    };
    for (const char* word : {"alert", "priority", "notice", "info", "connected", "disconnected"}) {
        size_t start = 0;
        size_t len = std::strlen(word);
        while (true) {
            size_t p = lower.find(word, start);
            if (p == std::string::npos) break;
            if ((p == 0 || isBoundary(p - 1)) && isBoundary(p + len)) add(p, len, 1);
            start = p + len;
        }
    }
    size_t p = 0;
    while ((p = lower.find("pc:", p)) != std::string::npos) {
        size_t e = p + 3;
        while (e < lower.size() && std::isdigit((unsigned char)lower[e])) ++e;
        add(p, e - p, 5);
        p = e;
    }
    return finish();
}

void SyntaxHighlighter::notifyEdit(size_t startByte, size_t oldEndByte, size_t newEndByte, size_t startRow, size_t startCol, size_t oldEndRow, size_t oldEndCol, size_t newEndRow, size_t newEndCol) {
    (void)startByte; (void)oldEndByte; (void)newEndByte;
    (void)startRow; (void)startCol; (void)oldEndRow; (void)oldEndCol; (void)newEndRow; (void)newEndCol;
}
