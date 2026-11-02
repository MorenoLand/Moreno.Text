// Sends editor events (load, activate, modify, save, close) to the plugin host so EventListener hooks run on real files.
#include "Core/Application.h"
#include "Plugin/PluginHost.h"
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

void Application::pollPluginEvents() {
    if (!pluginEventsChecked_) {
        pluginEventsChecked_ = true;
        pluginEventsEnabled_ = !PluginHost::discoverScripts(paths_.packagesDir).empty();
    }
    if (!pluginEventsEnabled_) return;
    if (binaryView_ || largeFileGuarded_) { pluginActiveKey_.clear(); return; }
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
