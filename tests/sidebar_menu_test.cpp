#include "UI/SidebarMenu.h"

int main() {
    auto root = sidebarMenuItems(true, true);
    if (root.empty() || root.front().label != "New File") return 1;
    if (root.back().label != "Remove Folder from Project") return 2;
    auto folder = sidebarMenuItems(true, false);
    if (folder.size() < 8 || folder[1].label != "Delete Folder") return 3;
    if (folder.back().label != "Find in Folder...") return 4;
    auto file = sidebarMenuItems(false, false);
    if (file.size() < 8 || file[1].label != "Delete File") return 5;
    if (file.back().label != "Blame File...") return 6;
    return 0;
}
