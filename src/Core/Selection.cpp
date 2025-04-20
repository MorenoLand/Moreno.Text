#include "Core/Selection.h"

void Selection::add(const Region& r) {
    for (auto& existing : regions_) {
        if (existing.overlaps(r)) {
            existing = Region(std::min(existing.begin, r.begin), std::max(existing.end, r.end));
            deduplicate();
            return;
        }
    }
    regions_.push_back(r);
    sort();
}

void Selection::set(const Region& r) {
    regions_.clear();
    regions_.push_back(r);
}

void Selection::sort() {
    std::sort(regions_.begin(), regions_.end(), [](const Region& a, const Region& b) { return a.begin < b.begin; });
}

void Selection::deduplicate() {
    if (regions_.size() <= 1) return;
    sort();
    std::vector<Region> merged;
    merged.push_back(regions_[0]);
    for (size_t i = 1; i < regions_.size(); ++i) {
        auto& last = merged.back();
        if (regions_[i].begin <= last.end) {
            last.end = std::max(last.end, regions_[i].end);
        } else {
            merged.push_back(regions_[i]);
        }
    }
    regions_ = std::move(merged);
}
