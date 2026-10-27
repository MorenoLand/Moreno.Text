#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <fstream>
#include <regex>
#include <nlohmann/json.hpp>

struct KeyBinding {
    std::vector<std::string> keys;
    std::string command;
    nlohmann::json args;
    nlohmann::json context;  // array of {key, operator, operand, match_all}; empty means always
};

class KeyBindingManager {
public:
    static KeyBindingManager& instance() {
        static KeyBindingManager k;
        return k;
    }
    void clear() { bindings_.clear(); }
    // Reads a keymap in Sublime's JSON format (comments and trailing commas allowed). Entries with a
    // "context" only fire while the context evaluator (setContextEvaluator) says every condition holds.
    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;
        std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB) text.erase(0, 3);
        static const std::regex trailingComma(R"(,(\s*[}\]]))");
        text = std::regex_replace(text, trailingComma, "$1");
        auto arr = nlohmann::json::parse(text, nullptr, false, true);
        if (!arr.is_array()) return;
        for (auto& entry : arr) {
            if (!entry.is_object()) continue;
            KeyBinding kb;
            if (entry.contains("keys") && entry["keys"].is_array()) {
                for (auto& k : entry["keys"]) if (k.is_string()) kb.keys.push_back(normalize(k.get<std::string>()));
            }
            if (entry.contains("command") && entry["command"].is_string()) kb.command = entry["command"].get<std::string>();
            if (entry.contains("args")) kb.args = entry["args"];
            if (entry.contains("context") && entry["context"].is_array()) kb.context = entry["context"];
            if (!kb.command.empty() && !kb.keys.empty()) bindings_.push_back(std::move(kb));
        }
    }
    void writeDefault(const std::string& path) {
        std::ofstream f(path);
        if (!f.is_open()) return;
        nlohmann::json j = {
            {{"keys", {"ctrl+n"}}, {"command", "new_file"}},
            {{"keys", {"ctrl+o"}}, {"command", "open_file"}},
            {{"keys", {"ctrl+s"}}, {"command", "save"}},
            {{"keys", {"ctrl+shift+s"}}, {"command", "save_as"}},
            {{"keys", {"ctrl+w"}}, {"command", "close_file"}},
            {{"keys", {"ctrl+shift+t"}}, {"command", "reopen_closed_tab"}},
            {{"keys", {"ctrl+z"}}, {"command", "undo"}},
            {{"keys", {"ctrl+y"}}, {"command", "redo"}},
            {{"keys", {"ctrl+x"}}, {"command", "cut"}},
            {{"keys", {"ctrl+c"}}, {"command", "copy"}},
            {{"keys", {"ctrl+v"}}, {"command", "paste"}},
            {{"keys", {"ctrl+shift+v"}}, {"command", "paste_and_indent"}},
            {{"keys", {"ctrl+a"}}, {"command", "select_all"}},
            {{"keys", {"ctrl+f"}}, {"command", "find"}},
            {{"keys", {"ctrl+h"}}, {"command", "replace"}},
            {{"keys", {"ctrl+p"}}, {"command", "goto_anything"}},
            {{"keys", {"ctrl+shift+p"}}, {"command", "command_palette"}},
            {{"keys", {"ctrl+d"}}, {"command", "find_under_expand"}},
            {{"keys", {"ctrl+l"}}, {"command", "select_line"}},
            {{"keys", {"ctrl+shift+l"}}, {"command", "split_selection_into_lines"}},
            {{"keys", {"ctrl+g"}}, {"command", "goto_line"}},
            {{"keys", {"ctrl+/"}}, {"command", "toggle_comment"}},
            {{"keys", {"ctrl+shift+/"}}, {"command", "toggle_block_comment"}},
            {{"keys", {"ctrl+shift+up"}}, {"command", "swap_line_up"}},
            {{"keys", {"ctrl+shift+down"}}, {"command", "swap_line_down"}},
            {{"keys", {"ctrl+shift+d"}}, {"command", "duplicate_line"}},
            {{"keys", {"ctrl+shift+k"}}, {"command", "delete_line"}},
            {{"keys", {"ctrl+shift+j"}}, {"command", "join_lines"}},
            {{"keys", {"ctrl+k", "ctrl+u"}}, {"command", "upper_case"}},
            {{"keys", {"ctrl+k", "ctrl+l"}}, {"command", "lower_case"}},
            {{"keys", {"f11"}}, {"command", "toggle_full_screen"}},
            {{"keys", {"alt+z"}}, {"command", "toggle_word_wrap"}},
            {{"keys", {"ctrl+f2"}}, {"command", "toggle_bookmark"}},
            {{"keys", {"f2"}}, {"command", "next_bookmark"}},
            {{"keys", {"shift+f2"}}, {"command", "prev_bookmark"}},
            {{"keys", {"ctrl+shift+f2"}}, {"command", "clear_bookmarks"}},
            {{"keys", {"ctrl+k", "ctrl+b"}}, {"command", "toggle_side_bar"}},
            {{"keys", {"ctrl+`"}}, {"command", "toggle_console"}}
        };
        f << j.dump(2);
    }
    void addCommandHandler(const std::string& cmd, std::function<void()> fn) {
        handlers_[cmd] = [fn = std::move(fn)](const nlohmann::json&) { fn(); };
    }
    void addCommandHandlerWithArgs(const std::string& cmd, std::function<void(const nlohmann::json&)> fn) {
        handlers_[cmd] = std::move(fn);
    }
    // Called for commands with no built-in handler (for example plugin commands). Returns true if it ran one.
    void setFallback(std::function<bool(const std::string&, const nlohmann::json&)> fn) { fallback_ = std::move(fn); }
    using ContextEvaluator = std::function<bool(const std::string& key, const std::string& op, const nlohmann::json& operand, bool matchAll)>;
    void setContextEvaluator(ContextEvaluator fn) { evaluator_ = std::move(fn); }
    bool dispatch(const std::string& keyCombo) const {
        for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
            if (it->keys.size() == 1 && it->keys[0] == keyCombo && contextHolds(*it)) return run(*it);
        return false;
    }
    bool hasChord(const std::string& firstKey) const {
        for (auto& kb : bindings_) {
            if (kb.keys.size() == 2 && kb.keys[0] == firstKey && contextHolds(kb)) return true;
        }
        return false;
    }
    bool dispatchChord(const std::string& firstKey, const std::string& secondKey) const {
        for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it)
            if (it->keys.size() == 2 && it->keys[0] == firstKey && it->keys[1] == secondKey && contextHolds(*it)) return run(*it);
        return false;
    }
    // Sublime spells some keys differently from the key names Moreno builds from SDL events.
    static std::string normalize(std::string key) {
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static const std::pair<const char*, const char*> aliases[] = {
            {"escape", "escape"}, {"enter", "enter"}, {"keypad_enter", "enter"}, {"plus", "="}, {"equals", "="}, {"minus", "-"},
            {"keypad_minus", "-"}, {"keypad_plus", "="}, {"keypad_multiply", "*"}, {"keypad_divide", "/"}};
        size_t plus = key.rfind('+');
        std::string prefix = plus == std::string::npos || plus + 1 >= key.size() ? std::string() : key.substr(0, plus + 1);
        std::string last = prefix.empty() ? key : key.substr(plus + 1);
        for (auto& [from, to] : aliases) if (last == from) { last = to; break; }
        return prefix + last;
    }
    const std::vector<KeyBinding>& bindings() const { return bindings_; }
private:
    KeyBindingManager() = default;
    std::vector<KeyBinding> bindings_;
    bool contextHolds(const KeyBinding& kb) const {
        if (!kb.context.is_array() || kb.context.empty()) return true;
        if (!evaluator_) return false;
        for (auto& c : kb.context) {
            if (!c.is_object() || !c.contains("key") || !c["key"].is_string()) return false;
            std::string op = c.value("operator", std::string("equal"));
            bool matchAll = c.value("match_all", false);
            if (!evaluator_(c["key"].get<std::string>(), op, c.contains("operand") ? c["operand"] : nlohmann::json(true), matchAll)) return false;
        }
        return true;
    }
    ContextEvaluator evaluator_;
    bool run(const KeyBinding& kb) const {
        auto it = handlers_.find(kb.command);
        if (it != handlers_.end()) { it->second(kb.args); return true; }
        return fallback_ ? fallback_(kb.command, kb.args) : false;
    }
    std::unordered_map<std::string, std::function<void(const nlohmann::json&)>> handlers_;
    std::function<bool(const std::string&, const nlohmann::json&)> fallback_;
};
