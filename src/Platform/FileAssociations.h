// Choosing which file extensions Moreno Text opens, in the spirit of 7-Zip's association panel.
// Everything is written under HKEY_CURRENT_USER, in keys that belong to Moreno Text or to the extension being
// changed, so no administrator rights are needed and nothing outside those keys is touched.
// The window itself is drawn by the editor (Core/ApplicationDialogs.cpp); this is only the registry side.
#pragma once
#include <string>
#include <vector>

namespace FileAssociations {
    struct Entry {
        std::string ext;         // lower case, no dot
        std::string opensWith;   // what Windows currently runs for it, for display
        bool checked = false;    // the box in the window
        bool registered = false; // what is registered right now
    };

    // The usual text, config and script types plus anything already registered.
    std::vector<Entry> load();
    // Re-reads `opensWith` for every entry.
    void refresh(std::vector<Entry>& entries);
    // "gs2", ".GS2" -> "gs2"; empty when the text cannot be a file extension.
    std::string normalize(const std::string& text);
    // Registers the ticked entries and releases the unticked ones that were registered. Returns a summary and any
    // notes about types where Windows keeps its own "Default apps" choice.
    std::string apply(std::vector<Entry>& entries);
    // Opens Windows' own Default apps page, the one place Windows lets the user change a default.
    void openDefaultAppsSettings();
}
