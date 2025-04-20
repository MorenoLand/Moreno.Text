#include "Core/Buffer.h"

Buffer::Buffer() {}
Buffer::Buffer(std::string text) : rope_(std::move(text)) {}

void Buffer::insert(size_t pos, std::string_view text) { rope_.insert(pos, text); }
void Buffer::erase(size_t pos, size_t len) { rope_.erase(pos, len); }

void Buffer::replace(size_t pos, size_t len, std::string_view text) {
    erase(pos, len);
    insert(pos, text);
}
