#include "Core/Rope.h"
// Full rope implementation coming in Phase 2
Rope::Rope(std::string text) : data_(std::move(text)) {}
void Rope::insert(size_t pos, const std::string& text) { data_.insert(pos, text); }
void Rope::erase(size_t pos, size_t len) { data_.erase(pos, len); }
std::string Rope::str() const { return data_; }
size_t Rope::size() const { return data_.size(); }
