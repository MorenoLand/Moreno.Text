#pragma once
#include <vector>
#include <algorithm>
#include <cassert>

struct Region {
    size_t begin = 0;
    size_t end = 0;
    Region() = default;
    Region(size_t pos) : begin(pos), end(pos) {}
    Region(size_t a, size_t b) : begin(std::min(a, b)), end(std::max(a, b)) {}
    size_t size() const { return end - begin; }
    bool empty() const { return begin == end; }
    bool contains(size_t pos) const { return pos >= begin && pos < end; }
    bool contains(const Region& r) const { return r.begin >= begin && r.end <= end; }
    bool overlaps(const Region& r) const { return begin < r.end && r.begin < end; }
    bool operator==(const Region& o) const { return begin == o.begin && end == o.end; }
    bool operator!=(const Region& o) const { return !(*this == o); }
    size_t cursor() const { return end; }
};

class Selection {
public:
    Selection() { regions_.emplace_back(0); }
    void add(const Region& r);
    void set(const Region& r);
    void clear() { regions_.clear(); regions_.emplace_back(0); }
    const std::vector<Region>& regions() const { return regions_; }
    size_t count() const { return regions_.size(); }
    const Region& primary() const { return regions_.back(); }
    Region& primary() { return regions_.back(); }
    void deduplicate();
    void sort();
private:
    std::vector<Region> regions_;
};
