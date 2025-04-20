#pragma once
#include "Core/Rope.h"
#include "Core/Selection.h"
#include <string>
#include <string_view>
#include <functional>
#include <vector>

struct BufferChange {
    enum Type { INSERT, ERASE } type;
    size_t pos;
    std::string text;
    std::string replaced;
};

class Buffer {
public:
    Buffer();
    explicit Buffer(std::string text);

    void insert(size_t pos, std::string_view text);
    void erase(size_t pos, size_t len);
    void replace(size_t pos, size_t len, std::string_view text);
    char charAt(size_t pos) const { return rope_.charAt(pos); }
    std::string substr(size_t pos, size_t len) const { return rope_.substr(pos, len); }
    std::string str() const { return rope_.str(); }

    size_t size() const { return rope_.size(); }
    size_t lineCount() const { return rope_.lineCount() + 1; }
    bool empty() const { return rope_.empty(); }

    size_t lineToOffset(size_t line) const { return rope_.lineToOffset(line); }
    size_t offsetToLine(size_t offset) const { return rope_.offsetToLine(offset); }
    void eachLine(size_t startLine, size_t endLine, std::function<void(size_t, std::string_view)> fn) const {
        rope_.lineRange(startLine, endLine, std::move(fn));
    }

    void beginEditGroup() { rope_.pushUndoGroup(); }
    bool canUndo() const { return rope_.canUndo(); }
    bool canRedo() const { return rope_.canRedo(); }
    void undo() { rope_.undo(); }
    void redo() { rope_.redo(); }

    const Rope& rope() const { return rope_; }
private:
    Rope rope_;
};
