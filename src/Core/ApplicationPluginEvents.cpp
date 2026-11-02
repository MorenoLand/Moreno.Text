// Sends editor events (load, activate, modify, save, close) to the plugin host so EventListener hooks run on real files.
#include "Core/Application.h"
#include "Plugin/PluginHost.h"
#include "UI/Titlebar.h"
#include <SDL2/SDL.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>

#ifdef None
#undef None
#endif

namespace fs = std::filesystem;

namespace {
constexpr size_t kMaxEventText = 4u * 1024u * 1024u;

// Plugins count code points (like Python strings); the editor counts UTF-8 bytes.
size_t byteToCp(const std::string& s, size_t byteOff) {
    byteOff = std::min(byteOff, s.size());
    size_t n = 0;
    for (size_t i = 0; i < byteOff; ++i) if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++n;
    return n;
}

size_t cpToByte(const std::string& s, size_t cp) {
    size_t i = 0;
    while (i < s.size() && cp > 0) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        --cp;
    }
    return i;
}
}

std::string Application::pluginEventKey(size_t tabIndex) const {
    if (tabIndex >= tabs_.size()) return {};
    const std::string& path = tabIndex == activeTab_ ? openFilePath_ : tabs_[tabIndex].filePath;
    if (path.rfind("plugin://", 0) == 0) return {};
    return path.empty() ? "untitled:" + std::to_string(tabIndex) : path;
}

// Writes one view_event request. The host deletes it once handled; content is left out when the text is not needed.
void Application::firePluginEvent(const char* event, const std::string& key, bool withContent) {
    if (key.empty() || !pluginEventsEnabled_) return;
    try {
        nlohmann::json j;
        j["type"] = "view_event";
        j["event"] = event;
        j["key"] = key;
        if (key.rfind("untitled:", 0) != 0) j["path"] = key;
        j["name"] = openFile_.empty() ? "untitled" : openFile_;
        j["dirty"] = dirty_;
        j["sel"] = nlohmann::json::array();
        for (auto& s : selections_) j["sel"].push_back({byteToCp(textBuffer, s.anchor), byteToCp(textBuffer, s.cursor)});
        if (withContent && textBuffer.size() <= kMaxEventText) j["content"] = textBuffer;
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        std::string stem = std::to_string(SDL_GetTicks()) + "-" + std::to_string(pluginEventSeq_++) + "-view_event";
        fs::path tmp = bridge / (stem + ".tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << j.dump();
        }
        std::error_code ec;
        fs::rename(tmp, bridge / (stem + ".request.json"), ec);
    } catch (...) {}
}

void Application::firePluginEvent(const char* event, bool withContent) {
    firePluginEvent(event, pluginEventKey(activeTab_), withContent);
}

// Sends on_hover after the mouse rests over the text for a moment (once per rest).
void Application::pollPluginHover() {
    uint32_t now = SDL_GetTicks();
    if (mouseX_ != hoverLastX_ || mouseY_ != hoverLastY_) { hoverLastX_ = mouseX_; hoverLastY_ = mouseY_; hoverSince_ = now; hoverSent_ = false; return; }
    if (hoverSent_ || now - hoverSince_ < 500 || goto_.active || commandPalette_.active || titlebar_->isMenuOpen()) return;
    hoverSent_ = true;
    int zone = 0;
    size_t offset = textOffsetAt(static_cast<float>(mouseX_), static_cast<float>(mouseY_), zone);
    if (zone == 0) return;
    std::string key = pluginEventKey(activeTab_);
    if (key.empty()) return;
    try {
        nlohmann::json j;
        j["type"] = "view_event";
        j["event"] = "on_hover";
        j["key"] = key;
        if (key.rfind("untitled:", 0) != 0) j["path"] = key;
        j["name"] = openFile_.empty() ? "untitled" : openFile_;
        j["dirty"] = dirty_;
        j["point"] = byteToCp(textBuffer, offset);
        j["zone"] = zone;
        j["sel"] = nlohmann::json::array();
        for (auto& s : selections_) j["sel"].push_back({byteToCp(textBuffer, s.anchor), byteToCp(textBuffer, s.cursor)});
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        std::string stem = std::to_string(SDL_GetTicks()) + "-" + std::to_string(pluginEventSeq_++) + "-view_event";
        fs::path tmp = bridge / (stem + ".tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << j.dump();
        }
        std::error_code ec;
        fs::rename(tmp, bridge / (stem + ".request.json"), ec);
    } catch (...) {}
}

void Application::pollPluginEvents() {
    if (!pluginEventsChecked_ || !pluginEventsEnabled_) return;  // set once the background plugin start has scanned the packages
    if (binaryView_ || largeFileGuarded_) { pluginActiveKey_.clear(); return; }
    pollPluginHover();
    std::string key = pluginEventKey(activeTab_);
    if (key.empty()) { pluginActiveKey_.clear(); return; }
    if (key != pluginActiveKey_) {
        if (!pluginActiveKey_.empty()) firePluginEvent("on_deactivated", pluginActiveKey_, false);
        bool first = pluginSeenKeys_.insert(key).second;
        pluginActiveKey_ = key;
        if (first) firePluginEvent(key.rfind("untitled:", 0) == 0 ? "on_new" : "on_load", key, true);
        firePluginEvent("on_activated", key, true);
        pluginTextHash_ = std::hash<std::string>{}(textBuffer);
        pluginSelections_ = selections_;
        lastPluginEventPoll_ = SDL_GetTicks();
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (now - lastPluginEventPoll_ < 250) return;
    lastPluginEventPoll_ = now;
    if (textBuffer.size() <= kMaxEventText) {
        size_t hash = std::hash<std::string>{}(textBuffer);
        if (hash != pluginTextHash_) {
            pluginTextHash_ = hash;
            firePluginEvent("on_modified", key, true);
        }
    }
    bool moved = selections_.size() != pluginSelections_.size();
    for (size_t i = 0; !moved && i < selections_.size(); ++i)
        moved = selections_[i].anchor != pluginSelections_[i].anchor || selections_[i].cursor != pluginSelections_[i].cursor;
    if (moved) {
        pluginSelections_ = selections_;
        firePluginEvent("on_selection_modified", key, false);
    }
}

// Applies a view_edit request from the plugin host: one replaced range, kept only if the old text still matches.
int Application::applyPluginViewEdit(const nlohmann::json& j) {
    std::string key = j.value("key", "");
    std::string oldText = j.value("old", ""), newText = j.value("new", "");
    size_t startCp = j.value("start", static_cast<size_t>(0));
    size_t tabIndex = tabs_.size();
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (pluginEventKey(i) == key) { tabIndex = i; break; }
    if (tabIndex == tabs_.size()) return 1;
    bool active = tabIndex == activeTab_;
    if (active && (binaryView_ || largeFileGuarded_)) return 2;
    std::string& text = active ? textBuffer : tabs_[tabIndex].text;
    size_t start = cpToByte(text, startCp);
    if (text.compare(start, oldText.size(), oldText) != 0 || start + oldText.size() > text.size()) return 3;
    if (active) {
        pushUndo();
        text.replace(start, oldText.size(), newText);
        if (j.contains("sel") && j["sel"].is_array() && !j["sel"].empty()) {
            selections_.clear();
            for (auto& r : j["sel"]) {
                if (!r.is_array() || r.size() != 2) continue;
                selections_.emplace_back(cpToByte(text, r[0].get<size_t>()), cpToByte(text, r[1].get<size_t>()));
            }
        }
        if (selections_.empty()) selections_.emplace_back(0);
        for (auto& s : selections_) { s.anchor = std::min(s.anchor, text.size()); s.cursor = std::min(s.cursor, text.size()); }
        dirty_ = true;
        invalidateLineIndex();
        syntaxDirty_ = true; indentsDirty_ = true;
    } else {
        text.replace(start, oldText.size(), newText);
        tabs_[tabIndex].dirty = true;
    }
    return 0;
}

namespace {
// Picks a colour for add_regions from its scope name (a stand-in for looking the scope up in the colour scheme).
void regionColor(const std::string& scope, float& r, float& g, float& b) {
    auto has = [&](const char* word) { return scope.find(word) != std::string::npos; };
    if (has("redish") || has("error") || has("invalid") || has("deleted")) { r = 0.90f; g = 0.30f; b = 0.30f; }
    else if (has("orangish") || has("warn")) { r = 0.95f; g = 0.60f; b = 0.20f; }
    else if (has("yellowish") || has("keyword") || has("comment")) { r = 0.95f; g = 0.80f; b = 0.25f; }
    else if (has("greenish") || has("string") || has("inserted")) { r = 0.40f; g = 0.78f; b = 0.40f; }
    else if (has("bluish") || has("entity")) { r = 0.35f; g = 0.60f; b = 0.95f; }
    else if (has("purplish") || has("constant")) { r = 0.65f; g = 0.45f; b = 0.90f; }
    else if (has("pinkish")) { r = 0.95f; g = 0.50f; b = 0.75f; }
    else { r = 0.60f; g = 0.70f; b = 0.80f; }
}
}  // namespace

// Stores a view.add_regions / erase_regions call from the plugin host; drawEditorHighlights paints them.
void Application::applyPluginRegions(const nlohmann::json& j) {
    std::string key = j.value("key", ""), name = j.value("name", "");
    if (key.empty() || name.empty()) return;
    if (j.value("erase", false)) {
        auto it = pluginRegions_.find(key);
        if (it != pluginRegions_.end()) it->second.erase(name);
        return;
    }
    const std::string* text = nullptr;
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (pluginEventKey(i) == key) { text = i == activeTab_ ? &textBuffer : &tabs_[i].text; break; }
    if (!text || !j.contains("regions") || !j["regions"].is_array()) return;
    PluginRegionSet set;
    set.flags = j.value("flags", 0);
    regionColor(j.value("scope", ""), set.r, set.g, set.b);
    for (auto& r : j["regions"]) {
        if (!r.is_array() || r.size() != 2 || set.ranges.size() >= 2000) continue;
        set.ranges.emplace_back(cpToByte(*text, r[0].get<size_t>()), cpToByte(*text, r[1].get<size_t>()));
    }
    pluginRegions_[key][name] = std::move(set);
}

// Asks the plugin host for on_query_completions results for the word being typed.
void Application::requestPluginCompletions() {
    if (!pluginEventsEnabled_ || selections_.empty() || binaryView_ || largeFileGuarded_) return;
    std::string key = pluginEventKey(activeTab_);
    if (key.empty()) return;
    try {
        nlohmann::json j;
        j["type"] = "completion_query";
        j["key"] = key;
        j["prefix"] = acPrefix_;
        j["point"] = byteToCp(textBuffer, selections_[0].cursor);
        if (textBuffer.size() <= 256u * 1024u) j["content"] = textBuffer;
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        std::string stem = std::to_string(SDL_GetTicks()) + "-" + std::to_string(pluginEventSeq_++) + "-completion_query";
        fs::path tmp = bridge / (stem + ".tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << j.dump();
        }
        std::error_code ec;
        fs::rename(tmp, bridge / (stem + ".request.json"), ec);
    } catch (...) {}
}

// Merges completions from the plugin host into the autocomplete popup if they still match what is being typed.
void Application::applyPluginCompletions(const nlohmann::json& j) {
    if (j.value("key", "") != pluginEventKey(activeTab_) || j.value("prefix", "") != acPrefix_ || acPrefix_.size() < 2) return;
    auto lower = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::string needle = lower(acPrefix_);
    std::vector<std::string> items, inserts;
    if (j.contains("items") && j["items"].is_array()) {
        for (auto& it : j["items"]) {
            if (!it.is_array() || it.size() < 2) continue;
            std::string label = it[0].get<std::string>(), contents = it[1].get<std::string>();
            std::string hay = lower(label);
            size_t at = 0;
            for (char c : needle) { at = hay.find(c, at); if (at == std::string::npos) break; ++at; }
            if (at == std::string::npos) continue;
            items.push_back(label);
            inserts.push_back(contents);
        }
    }
    if (items.empty()) return;
    if (!j.value("inhibit", false)) {
        for (size_t i = 0; i < acItems_.size(); ++i) {
            if (std::find(items.begin(), items.end(), acItems_[i]) != items.end()) continue;
            items.push_back(acItems_[i]);
            inserts.push_back(i < acInsert_.size() ? acInsert_[i] : std::string());
        }
    }
    if (items.size() > 10) { items.resize(10); inserts.resize(10); }
    bool wasActive = acActive_;
    acItems_ = std::move(items);
    acInsert_ = std::move(inserts);
    acActive_ = true;
    if (!wasActive || acSelected_ >= static_cast<int>(acItems_.size())) acSelected_ = 0;
}

// Asks plugins' on_query_context about a keymap context key we don't know. Waits briefly for the host; answers are
// cached for a moment and the host is skipped for a few seconds after it fails to answer in time.
bool Application::pluginQueryContext(const std::string& key, const std::string& op, const nlohmann::json& operand, bool matchAll) {
    if (!pluginEventsEnabled_) return false;
    std::string viewKey = pluginEventKey(activeTab_);
    if (viewKey.empty()) return false;
    uint32_t now = SDL_GetTicks();
    std::string cacheKey = key + "|" + op + "|" + operand.dump() + "|" + std::to_string(matchAll) + "|" + viewKey;
    auto cached = pluginContextCache_.find(cacheKey);
    if (cached != pluginContextCache_.end() && now - cached->second.first < 400) return cached->second.second;
    if (now < pluginContextDownUntil_) return false;
    bool result = false;
    try {
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        std::string stem = std::to_string(now) + "-" + std::to_string(pluginEventSeq_++) + "-query_context";
        fs::path response = bridge / (stem + ".response.json");
        nlohmann::json j = {{"type", "query_context"}, {"key", viewKey}, {"context", key}, {"operator", op}, {"operand", operand},
                            {"match_all", matchAll}, {"response", response.string()}};
        fs::path tmp = bridge / (stem + ".tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << j.dump();
        }
        std::error_code ec;
        fs::rename(tmp, bridge / (stem + ".request.json"), ec);
        bool answered = false;
        for (int i = 0; i < 40 && !answered; ++i) {
            if (fs::exists(response, ec)) {
                std::ifstream in(response, std::ios::binary);
                auto parsed = nlohmann::json::parse(in, nullptr, false);
                if (parsed.is_object()) { result = parsed.value("result", false); answered = true; }
            }
            if (!answered) SDL_Delay(2);
        }
        fs::remove(response, ec);
        if (!answered) pluginContextDownUntil_ = SDL_GetTicks() + 5000;
    } catch (...) {}
    pluginContextCache_[cacheKey] = {SDL_GetTicks(), result};
    if (pluginContextCache_.size() > 200) pluginContextCache_.clear();
    return result;
}

// Window-level events (on_new_window, on_load_project, ...) for plugins.
void Application::firePluginWindowEvent(const char* event) {
    if (!pluginEventsEnabled_) return;
    try {
        fs::path bridge = fs::path(paths_.localDir) / "PluginBridge";
        fs::create_directories(bridge);
        std::string stem = std::to_string(SDL_GetTicks()) + "-" + std::to_string(pluginEventSeq_++) + "-window_event";
        fs::path tmp = bridge / (stem + ".tmp");
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            f << nlohmann::json{{"type", "window_event"}, {"event", event}}.dump();
        }
        std::error_code ec;
        fs::rename(tmp, bridge / (stem + ".request.json"), ec);
    } catch (...) {}
}
