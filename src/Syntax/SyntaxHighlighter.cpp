#include "Syntax/SyntaxHighlighter.h"
#include "Theme/ThemeEngine.h"
#include "api.h"
#include <cctype>
#include <algorithm>
#include <cstring>
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

void SyntaxHighlighter::setLanguage(const std::string& ext) {
    keywords_.clear(); builtins_.clear(); types_.clear();
    { std::lock_guard<std::mutex> lock(tokenMutex_); treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>(); }
    language_ = nullptr;
    if (ext == "js" || ext == "jsx" || ext == "ts" || ext == "tsx" || ext == "mjs") { setupJS(); setTreeSitterLanguage("JavaScript", tree_sitter_javascript()); }
    else if (ext == "py" || ext == "pyw") { setupPython(); setTreeSitterLanguage("Python", tree_sitter_python()); }
    else if (ext == "c") { setupCPP(); setTreeSitterLanguage("C", tree_sitter_c()); }
    else if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" || ext == "hxx") { setupCPP(); setTreeSitterLanguage("C++", tree_sitter_cpp()); }
    else if (ext == "json") { setupPlainText(); langName_ = "JSON"; language_ = nullptr; }
    else if (ext == "md" || ext == "markdown") { setupPlainText(); setTreeSitterLanguage("Markdown", tree_sitter_markdown()); }
    else if (ext == "xml") { setupPlainText(); langName_ = "XML"; }
    else if (ext == "yml" || ext == "yaml") { setupPlainText(); langName_ = "YAML"; }
    else if (ext == "toml") { setupPlainText(); langName_ = "TOML"; }
    else if (ext == "html" || ext == "htm") { setupPlainText(); langName_ = "HTML"; }
    else if (ext == "css") { setupPlainText(); langName_ = "CSS"; }
    else setupPlainText();
}

void SyntaxHighlighter::setLanguageByName(const std::string& name) {
    static const struct { const char* name; const char* ext; } map[] = {
        {"JavaScript","js"},{"Python","py"},{"C++","cpp"},{"C","c"},{"HTML","html"},{"CSS","css"},
        {"JSON","json"},{"Java","java"},{"Go","go"},{"Rust","rs"},{"Ruby","rb"},{"PHP","php"},
        {"SQL","sql"},{"Lua","lua"},{"Markdown","md"},{"XML","xml"},{"YAML","yml"},{"TOML","toml"},
        {"C#","cs"},{"Objective-C","m"},{"Swift","swift"},{"TypeScript","ts"},{"ShellScript","sh"},{"Batch File","bat"},{"Plain Text",""},
    };
    for (auto& m : map) if (name == m.name) { setLanguage(m.ext); return; }
    keywords_.clear(); builtins_.clear(); types_.clear();
    { std::lock_guard<std::mutex> lock(tokenMutex_); treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>(); }
    language_ = nullptr;
    setupGenericCode(name);
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

void SyntaxHighlighter::ensureParseWorker() {
    if (!parseWorker_.joinable()) parseWorker_ = std::thread(&SyntaxHighlighter::parseWorkerLoop, this);
}

void SyntaxHighlighter::parseWorkerLoop() {
    for (;;) {
        std::string text;
        const void* language = nullptr;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(parseMutex_);
            parseCv_.wait(lock, [&] { return parseStop_ || parseRequested_; });
            if (parseStop_) return;
            text = std::move(pendingText_);
            language = pendingLanguage_;
            generation = pendingGeneration_;
            parseRequested_ = false;
        }
        auto parsed = std::make_shared<const std::vector<SyntaxToken>>(parseTreeTokens(text, language));
        if (generation == latestGeneration_.load()) {
            std::lock_guard<std::mutex> lock(tokenMutex_);
            treeTokens_ = std::move(parsed);
        }
    }
}

void SyntaxHighlighter::parse(const std::string& text) {
    if (!language_) {
        std::lock_guard<std::mutex> lock(tokenMutex_);
        treeTokens_ = std::make_shared<const std::vector<SyntaxToken>>();
        return;
    }
    ensureParseWorker();
    {
        std::lock_guard<std::mutex> lock(parseMutex_);
        pendingText_ = text;
        pendingLanguage_ = language_;
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

const SyntaxColor& SyntaxHighlighter::scopeColor(int scope) const {
    static const char* scopeNames[] = {"","keyword","string","comment","number","type","function","operator","punctuation"};
    static thread_local SyntaxColor cached[9];
    if (scope >= 1 && scope < 9) {
        ThemeColor tc = ThemeEngine::instance().colorForScope(scopeNames[scope]);
        cached[scope] = {tc.r, tc.g, tc.b}; return cached[scope];
    }
    ThemeColor tc = ThemeEngine::instance().fgColor();
    cached[0] = {tc.r, tc.g, tc.b}; return cached[0];
}

void SyntaxHighlighter::notifyEdit(size_t startByte, size_t oldEndByte, size_t newEndByte, size_t startRow, size_t startCol, size_t oldEndRow, size_t oldEndCol, size_t newEndRow, size_t newEndCol) {
    (void)startByte; (void)oldEndByte; (void)newEndByte;
    (void)startRow; (void)startCol; (void)oldEndRow; (void)oldEndCol; (void)newEndRow; (void)newEndCol;
}
