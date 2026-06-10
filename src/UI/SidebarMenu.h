#pragma once
#include <string>
#include <vector>

struct SidebarMenuItem {
    std::string label;
    int action = -1;
    bool separator = false;
};

inline std::vector<SidebarMenuItem> sidebarMenuItems(bool folder, bool root) {
    std::vector<SidebarMenuItem> items;
    if (root) {
        items.push_back({"New File", 0});
        items.push_back({"Rename...", 1});
        items.push_back({"Open Folder", 2});
        items.push_back({"Open Terminal Here...", 3});
        items.push_back({"Copy Path", 4});
        items.push_back({"", -1, true});
        items.push_back({"Open Git Repository...", 5});
        items.push_back({"Folder History...", 6});
        items.push_back({"", -1, true});
        items.push_back({"New Folder", 7});
        items.push_back({"Delete Folder", 8});
        items.push_back({"Find in Folder...", 9});
        items.push_back({"Remove Folder from Project", 10});
        return items;
    }
    items.push_back({"Rename...", 1});
    items.push_back({folder ? "Delete Folder" : "Delete File", 8});
    items.push_back({"Open Containing Folder...", 11});
    items.push_back({"Copy Path", 4});
    items.push_back({"", -1, true});
    items.push_back({"Open Git Repository...", 5});
    items.push_back({folder ? "Folder History..." : "File History...", 6});
    items.push_back({folder ? "Find in Folder..." : "Blame File...", folder ? 9 : 12});
    return items;
}
