#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

struct KeyBinding {
    std::vector<std::string> keys;
    std::string command;
    nlohmann::json args;
};

class KeyBindingManager {
public:
    static KeyBindingManager& instance() {
        static KeyBindingManager k;
        return k;
    }
    void load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        try {
            auto arr = nlohmann::json::parse(f);
            if (!arr.is_array()) return;
            for (auto& entry : arr) {
                KeyBinding kb;
                if (entry.contains("keys") && entry["keys"].is_array()) {
                    for (auto& k : entry["keys"]) kb.keys.push_back(k.get<std::string>());
                }
                if (entry.contains("command")) kb.command = entry["command"].get<std::string>();
                if (entry.contains("args")) kb.args = entry["args"];
                if (!kb.command.empty()) bindings_.push_back(std::move(kb));
            }
        } catch (...) {}
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
        handlers_[cmd] = std::move(fn);
    }
    bool dispatch(const std::string& keyCombo) const {
        for (auto& kb : bindings_) {
            if (kb.keys.size() == 1 && kb.keys[0] == keyCombo) {
                auto it = handlers_.find(kb.command);
                if (it != handlers_.end()) { it->second(); return true; }
                return false;
            }
        }
        return false;
    }
    bool hasChord(const std::string& firstKey) const {
        for (auto& kb : bindings_) {
            if (kb.keys.size() == 2 && kb.keys[0] == firstKey) return true;
        }
        return false;
    }
    bool dispatchChord(const std::string& firstKey, const std::string& secondKey) const {
        for (auto& kb : bindings_) {
            if (kb.keys.size() == 2 && kb.keys[0] == firstKey && kb.keys[1] == secondKey) {
                auto it = handlers_.find(kb.command);
                if (it != handlers_.end()) { it->second(); return true; }
                return false;
            }
        }
        return false;
    }
    const std::vector<KeyBinding>& bindings() const { return bindings_; }
private:
    KeyBindingManager() = default;
    std::vector<KeyBinding> bindings_;
    std::unordered_map<std::string, std::function<void()>> handlers_;
};
