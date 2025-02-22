#pragma once
#include <string>

class Rope {
public:
    Rope() = default;
    explicit Rope(std::string text);
    void insert(size_t pos, const std::string& text);
    void erase(size_t pos, size_t len);
    std::string str() const;
    size_t size() const;
private:
    std::string data_;
};
