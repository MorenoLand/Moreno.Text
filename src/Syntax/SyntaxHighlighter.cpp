#include "Syntax/SyntaxHighlighter.h"
#include <cctype>
#include <algorithm>

SyntaxHighlighter::SyntaxHighlighter() { setupPlainText(); }

void SyntaxHighlighter::setLanguage(const std::string& ext) {
    keywords_.clear(); builtins_.clear(); types_.clear();
    if (ext == "js" || ext == "jsx" || ext == "ts" || ext == "tsx" || ext == "mjs") setupJS();
    else if (ext == "py" || ext == "pyw") setupPython();
    else if (ext == "c" || ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "h" || ext == "hpp" || ext == "hxx") setupCPP();
    else if (ext == "json") { setupPlainText(); langName_ = "JSON"; }
    else if (ext == "md" || ext == "markdown") { setupPlainText(); langName_ = "Markdown"; }
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
    langName_ = name;
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
void SyntaxHighlighter::setupPlainText() { langName_ = "Plain Text"; }

static bool isWordChar(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::vector<SyntaxToken> SyntaxHighlighter::highlightLine(std::string_view line, size_t lineOffset) const {
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
    return colors_[(scope >= 0 && scope < 9) ? scope : 0];
}
