#include "UI/Minimap.h"
#include <cassert>

int main() {
    auto small = Minimap::visibleLineWindow(100, 240.f, 4.f);
    assert(small.first == 0);
    assert(small.count == 100);
    auto large = Minimap::visibleLineWindow(1000000, 240.f, 4.f);
    assert(large.first == 0);
    assert(large.count < 1000000);
    assert(large.count >= 60);
    return 0;
}
