#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <algorithm>
#include <cassert>

class Rope {
public:
    static constexpr size_t LEAF_MAX = 512;
    static constexpr size_t LEAF_MIN = 64;

    Rope();
    explicit Rope(std::string text);
    Rope(const Rope& other);
    Rope(Rope&& other) noexcept;
    Rope& operator=(const Rope& other);
    Rope& operator=(Rope&& other) noexcept;

    void insert(size_t pos, std::string_view text);
    void erase(size_t pos, size_t len);
    char charAt(size_t pos) const;
    size_t size() const { return root_ ? root_->weight : 0; }
    size_t lineCount() const { return root_ ? root_->lines : 0; }
    bool empty() const { return size() == 0; }

    std::string str() const;
    std::string substr(size_t pos, size_t len) const;

    size_t lineToOffset(size_t line) const;
    size_t offsetToLine(size_t offset) const;
    std::string_view lineAt(size_t lineIndex, size_t& outLen) const;
    void lineRange(size_t startLine, size_t endLine, std::function<void(size_t, std::string_view)> callback) const;

    void pushUndoGroup();
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    void setUndoGroupWindow(std::chrono::milliseconds ms) { undoGroupWindow_ = ms; }

private:
    struct Node {
        size_t weight = 0;
        size_t lines = 0;
        std::string leaf;
        std::unique_ptr<Node> left, right;
        bool isLeaf() const { return !left && !right; }
    };

    std::unique_ptr<Node> root_;
    std::chrono::milliseconds undoGroupWindow_{500};

    static std::unique_ptr<Node> makeLeaf(std::string text);
    static std::unique_ptr<Node> makeInner(std::unique_ptr<Node> l, std::unique_ptr<Node> r);
    static size_t nodeWeight(const std::unique_ptr<Node>& n) { return n ? n->weight : 0; }
    static size_t nodeLines(const std::unique_ptr<Node>& n) { return n ? n->lines : 0; }
    static void recompute(Node& n);
    static size_t countLines(std::string_view s);

    std::unique_ptr<Node> clone(const std::unique_ptr<Node>& n) const;
    std::unique_ptr<Node> buildTree(std::string_view text) const;
    std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>> split(std::unique_ptr<Node> node, size_t pos);
    std::unique_ptr<Node> merge(std::unique_ptr<Node> a, std::unique_ptr<Node> b);
    std::unique_ptr<Node> rebalance(std::unique_ptr<Node> node);

    struct EditOp { enum { INSERT, ERASE } type; size_t pos; std::string text; };
    struct UndoGroup { std::vector<EditOp> ops; std::chrono::steady_clock::time_point timestamp; };
    std::vector<UndoGroup> undoStack_;
    std::vector<UndoGroup> redoStack_;
    UndoGroup currentGroup_;
    bool grouping_ = false;

    void recordInsert(size_t pos, std::string_view text);
    void recordErase(size_t pos, std::string_view text);

    static void collectLeaves(const std::unique_ptr<Node>& n, std::vector<const Node*>& out);
    static size_t depth(const std::unique_ptr<Node>& n);
    std::unique_ptr<Node> rebalanceByLeaves(std::unique_ptr<Node> node);
};
