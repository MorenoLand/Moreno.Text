// Undo history that outlives a tab and a restart: written when a tab closes and on exit, read back
// when the same text is loaded again.
#include "Core/Application.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
constexpr char kMagic[4] = {'M', 'T', 'U', '1'};
constexpr uint64_t kBudgetBytes = 48ull * 1024ull * 1024ull;   // snapshot text kept per file

void putU64(std::ostream& os, uint64_t v) { os.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
bool getU64(std::istream& is, uint64_t& v) { return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(v))); }

// Newest steps that fit in the budget, kept in their original order.
std::vector<const UndoStep*> newestWithin(const std::vector<UndoStep>& steps, uint64_t& budget) {
    std::vector<const UndoStep*> kept;
    for (size_t i = steps.size(); i-- > 0;) {
        if (!steps[i].text) continue;
        uint64_t size = steps[i].text->size();
        if (size > budget) break;
        budget -= size;
        kept.push_back(&steps[i]);
    }
    std::reverse(kept.begin(), kept.end());
    return kept;
}

bool readSteps(std::istream& is, uint64_t count, uint64_t& remaining, std::vector<UndoStep>& out) {
    auto old = std::chrono::steady_clock::now() - std::chrono::hours(1);
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t cursor = 0, len = 0;
        if (!getU64(is, cursor) || !getU64(is, len) || len > remaining) return false;
        auto text = std::make_shared<std::string>(static_cast<size_t>(len), '\0');
        if (len && !is.read(text->data(), static_cast<std::streamsize>(len))) return false;
        remaining -= len;
        out.push_back({std::move(text), static_cast<size_t>(cursor), old});
    }
    return true;
}
}

std::string Application::undoStoreFile(const std::string& path) const {
    std::string key = path;
    for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c == '/' ? '\\' : c)));
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.undo", static_cast<unsigned long long>(std::hash<std::string>{}(key)));
    return (fs::path(paths_.localDir) / "undo" / name).string();
}

void Application::writeUndoStore(const TabBuffer& tab, const std::string& file) const {
    std::error_code ec;
    if (tab.undoStack.empty() && tab.redoStack.empty()) { fs::remove(file, ec); return; }
    fs::create_directories(fs::path(file).parent_path(), ec);
    uint64_t budget = kBudgetBytes;
    auto undo = newestWithin(tab.undoStack, budget);
    auto redo = newestWithin(tab.redoStack, budget);
    std::string tmp = file + ".tmp";
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return;
        os.write(kMagic, sizeof(kMagic));
        putU64(os, std::hash<std::string>{}(tab.text));
        putU64(os, tab.savedHash);
        putU64(os, undo.size());
        putU64(os, redo.size());
        for (auto* list : {&undo, &redo})
            for (auto* step : *list) { putU64(os, step->cursorPos); putU64(os, step->text->size()); os.write(step->text->data(), static_cast<std::streamsize>(step->text->size())); }
        if (!os) { os.close(); fs::remove(tmp, ec); return; }
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
}

bool Application::readUndoStore(TabBuffer& tab, const std::string& file) const {
    std::ifstream is(file, std::ios::binary);
    if (!is) return false;
    char magic[4] = {};
    uint64_t baseHash = 0, savedHash = 0, nUndo = 0, nRedo = 0;
    if (!is.read(magic, sizeof(magic)) || std::string(magic, 4) != std::string(kMagic, 4)) return false;
    if (!getU64(is, baseHash) || !getU64(is, savedHash) || !getU64(is, nUndo) || !getU64(is, nRedo)) return false;
    if (baseHash != std::hash<std::string>{}(tab.text)) return false;   // the text changed since: the history no longer applies
    std::error_code ec;
    uint64_t remaining = fs::file_size(file, ec);
    if (ec || nUndo > remaining || nRedo > remaining) return false;
    std::vector<UndoStep> undo, redo;
    if (!readSteps(is, nUndo, remaining, undo) || !readSteps(is, nRedo, remaining, redo)) return false;
    tab.undoStack = std::move(undo);
    tab.redoStack = std::move(redo);
    tab.savedHash = static_cast<size_t>(savedHash);
    return true;
}

// Brings back the history stored for the active tab's file when its text is exactly what the history ended on.
void Application::restoreUndoFor(const std::string& path) {
    if (path.empty() || activeTab_ >= tabs_.size() || binaryView_ || largeFileGuarded_) return;
    auto& tab = tabs_[activeTab_];
    tab.text = textBuffer;
    if (!readUndoStore(tab, undoStoreFile(path))) { tab.undoStack.clear(); tab.redoStack.clear(); return; }
    undoStack_ = tab.undoStack; redoStack_ = tab.redoStack; savedHash_ = tab.savedHash;
}
