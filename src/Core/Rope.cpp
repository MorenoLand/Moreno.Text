#include "Core/Rope.h"
#include <algorithm>
#include <queue>
#include <cstring>

Rope::Rope() : root_(makeLeaf("")) {}
Rope::Rope(std::string text) : root_(buildTree(text)) {}
Rope::Rope(const Rope& o) : root_(o.clone(o.root_)), undoGroupWindow_(o.undoGroupWindow_) {}
Rope::Rope(Rope&& o) noexcept : root_(std::move(o.root_)), undoStack_(std::move(o.undoStack_)), redoStack_(std::move(o.redoStack_)), undoGroupWindow_(o.undoGroupWindow_) {}
Rope& Rope::operator=(const Rope& o) { root_ = o.clone(o.root_); undoGroupWindow_ = o.undoGroupWindow_; return *this; }
Rope& Rope::operator=(Rope&& o) noexcept { root_ = std::move(o.root_); undoStack_ = std::move(o.undoStack_); redoStack_ = std::move(o.redoStack_); return *this; }

size_t Rope::countLines(std::string_view s) {
    return static_cast<size_t>(std::count(s.begin(), s.end(), '\n'));
}

void Rope::recompute(Node& n) {
    if (n.isLeaf()) {
        n.weight = n.leaf.size();
        n.lines = countLines(n.leaf);
        return;
    }
    n.weight = nodeWeight(n.left) + nodeWeight(n.right);
    n.lines = nodeLines(n.left) + nodeLines(n.right);
}

auto Rope::makeLeaf(std::string text) -> std::unique_ptr<Node> {
    auto n = std::make_unique<Node>();
    n->leaf = std::move(text);
    n->weight = n->leaf.size();
    n->lines = countLines(n->leaf);
    return n;
}

auto Rope::makeInner(std::unique_ptr<Node> l, std::unique_ptr<Node> r) -> std::unique_ptr<Node> {
    auto n = std::make_unique<Node>();
    n->left = std::move(l);
    n->right = std::move(r);
    recompute(*n);
    return n;
}

auto Rope::clone(const std::unique_ptr<Node>& n) const -> std::unique_ptr<Node> {
    if (!n) return nullptr;
    auto c = std::make_unique<Node>();
    c->weight = n->weight;
    c->lines = n->lines;
    c->leaf = n->leaf;
    if (n->left) c->left = clone(n->left);
    if (n->right) c->right = clone(n->right);
    return c;
}

auto Rope::buildTree(std::string_view text) const -> std::unique_ptr<Node> {
    if (text.size() <= LEAF_MAX) return makeLeaf(std::string(text));
    size_t mid = text.size() / 2;
    auto left = buildTree(text.substr(0, mid));
    auto right = buildTree(text.substr(mid));
    return makeInner(std::move(left), std::move(right));
}

auto Rope::split(std::unique_ptr<Node> node, size_t pos) -> std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>> {
    if (!node) return {nullptr, nullptr};
    if (node->isLeaf()) {
        if (pos >= node->leaf.size()) return {std::move(node), nullptr};
        if (pos == 0) return {nullptr, std::move(node)};
        auto l = makeLeaf(node->leaf.substr(0, pos));
        auto r = makeLeaf(node->leaf.substr(pos));
        return {std::move(l), std::move(r)};
    }
    size_t lw = nodeWeight(node->left);
    if (pos <= lw) {
        auto [ll, lr] = split(std::move(node->left), pos);
        auto rightTree = lr ? makeInner(std::move(lr), std::move(node->right)) : std::move(node->right);
        return {std::move(ll), std::move(rightTree)};
    } else {
        auto [rl, rr] = split(std::move(node->right), pos - lw);
        auto leftTree = rl ? makeInner(std::move(node->left), std::move(rl)) : std::move(node->left);
        return {std::move(leftTree), std::move(rr)};
    }
}

auto Rope::merge(std::unique_ptr<Node> a, std::unique_ptr<Node> b) -> std::unique_ptr<Node> {
    if (!a) return b;
    if (!b) return a;
    if (a->isLeaf() && b->isLeaf() && a->leaf.size() + b->leaf.size() <= LEAF_MAX) {
        a->leaf += b->leaf;
        recompute(*a);
        return a;
    }
    return makeInner(std::move(a), std::move(b));
}

size_t Rope::depth(const std::unique_ptr<Node>& n) {
    if (!n || n->isLeaf()) return 0;
    return 1 + std::max(depth(n->left), depth(n->right));
}

void Rope::collectLeaves(const std::unique_ptr<Node>& n, std::vector<const Node*>& out) {
    if (!n) return;
    if (n->isLeaf()) { out.push_back(n.get()); return; }
    collectLeaves(n->left, out);
    collectLeaves(n->right, out);
}

auto Rope::rebalanceByLeaves(std::unique_ptr<Node> node) -> std::unique_ptr<Node> {
    std::vector<const Node*> leaves;
    collectLeaves(node, leaves);
    if (leaves.empty()) return node;
    std::vector<std::unique_ptr<Node>> newLeaves;
    newLeaves.reserve(leaves.size());
    for (auto* l : leaves) newLeaves.push_back(makeLeaf(l->leaf));
    while (newLeaves.size() > 1) {
        std::vector<std::unique_ptr<Node>> next;
        for (size_t i = 0; i < newLeaves.size(); i += 2) {
            if (i + 1 < newLeaves.size()) {
                next.push_back(makeInner(std::move(newLeaves[i]), std::move(newLeaves[i + 1])));
            } else {
                next.push_back(std::move(newLeaves[i]));
            }
        }
        newLeaves = std::move(next);
    }
    return std::move(newLeaves[0]);
}

auto Rope::rebalance(std::unique_ptr<Node> node) -> std::unique_ptr<Node> {
    if (depth(node) > 64) return rebalanceByLeaves(std::move(node));
    return node;
}

void Rope::insert(size_t pos, std::string_view text) {
    if (text.empty()) return;
    pos = std::min(pos, size());
    recordInsert(pos, text);
    auto newSub = buildTree(text);
    auto [left, right] = split(std::move(root_), pos);
    root_ = rebalance(merge(merge(std::move(left), std::move(newSub)), std::move(right)));
}

void Rope::erase(size_t pos, size_t len) {
    size_t sz = size();
    if (pos >= sz || len == 0) return;
    len = std::min(len, sz - pos);
    auto erased = substr(pos, len);
    recordErase(pos, erased);
    auto [left, rest] = split(std::move(root_), pos);
    auto [_, right] = split(std::move(rest), len);
    root_ = rebalance(merge(std::move(left), std::move(right)));
}

char Rope::charAt(size_t pos) const {
    assert(pos < size());
    Node* cur = root_.get();
    while (cur && !cur->isLeaf()) {
        size_t lw = nodeWeight(cur->left);
        if (pos < lw) cur = cur->left.get();
        else { pos -= lw; cur = cur->right.get(); }
    }
    return cur ? cur->leaf[pos] : '\0';
}

std::string Rope::str() const { return substr(0, size()); }

std::string Rope::substr(size_t pos, size_t len) const {
    size_t sz = size();
    if (pos >= sz) return "";
    len = std::min(len, sz - pos);
    std::string result;
    result.reserve(len);
    std::function<void(const std::unique_ptr<Node>&, size_t, size_t)> collect =
        [&](const std::unique_ptr<Node>& n, size_t start, size_t end) {
        if (!n || start >= end) return;
        if (n->isLeaf()) {
            size_t s = std::max(start, size_t(0));
            size_t e = std::min(end, n->leaf.size());
            if (s < e) result.append(n->leaf, s, e - s);
            return;
        }
        size_t lw = nodeWeight(n->left);
        if (start < lw) collect(n->left, start, std::min(end, lw));
        if (end > lw) collect(n->right, start > lw ? start - lw : 0, end - lw);
    };
    collect(root_, pos, pos + len);
    return result;
}

size_t Rope::lineToOffset(size_t line) const {
    if (!root_ || line == 0) return 0;
    size_t offset = 0, linesLeft = line;
    Node* cur = root_.get();
    while (cur && linesLeft > 0) {
        if (cur->isLeaf()) {
            for (size_t i = 0; i < cur->leaf.size() && linesLeft > 0; ++i) {
                if (cur->leaf[i] == '\n') { --linesLeft; if (linesLeft == 0) return offset + i + 1; }
            }
            offset += cur->leaf.size();
            return offset;
        }
        size_t ll = nodeLines(cur->left);
        if (linesLeft <= ll) {
            cur = cur->left.get();
        } else {
            linesLeft -= ll;
            offset += nodeWeight(cur->left);
            cur = cur->right.get();
        }
    }
    return offset;
}

size_t Rope::offsetToLine(size_t offset) const {
    if (!root_ || offset == 0) return 0;
    size_t line = 0, offLeft = offset;
    Node* cur = root_.get();
    while (cur) {
        if (cur->isLeaf()) {
            size_t end = std::min(offLeft, cur->leaf.size());
            line += static_cast<size_t>(std::count(cur->leaf.begin(), cur->leaf.begin() + static_cast<ptrdiff_t>(end), '\n'));
            return line;
        }
        size_t lw = nodeWeight(cur->left);
        if (offLeft <= lw) {
            cur = cur->left.get();
        } else {
            line += nodeLines(cur->left);
            offLeft -= lw;
            cur = cur->right.get();
        }
    }
    return line;
}

std::string_view Rope::lineAt(size_t lineIndex, size_t& outLen) const {
    size_t start = lineToOffset(lineIndex);
    size_t sz = size();
    if (start >= sz) { outLen = 0; return {}; }
    size_t end = start;
    while (end < sz && charAt(end) != '\n') ++end;
    if (end < sz) ++end;
    outLen = end - start;
    thread_local std::string buf;
    buf = substr(start, outLen);
    return buf;
}

void Rope::lineRange(size_t startLine, size_t endLine, std::function<void(size_t, std::string_view)> callback) const {
    size_t sz = size();
    for (size_t line = startLine; line <= endLine; ++line) {
        size_t offset = lineToOffset(line);
        if (offset >= sz) break;
        size_t end = offset;
        while (end < sz && charAt(end) != '\n') ++end;
        std::string text = substr(offset, end - offset);
        callback(line, text);
    }
}

void Rope::pushUndoGroup() {
    if (!currentGroup_.ops.empty()) {
        undoStack_.push_back(std::move(currentGroup_));
        currentGroup_ = {};
        redoStack_.clear();
    }
    grouping_ = true;
}

bool Rope::canUndo() const { return !undoStack_.empty() || !currentGroup_.ops.empty(); }
bool Rope::canRedo() const { return !redoStack_.empty(); }

void Rope::undo() {
    if (undoStack_.empty() && currentGroup_.ops.empty()) return;
    if (!currentGroup_.ops.empty()) {
        undoStack_.push_back(std::move(currentGroup_));
        currentGroup_ = {};
    }
    auto& group = undoStack_.back();
    UndoGroup redoGroup;
    redoGroup.timestamp = group.timestamp;
    for (auto it = group.ops.rbegin(); it != group.ops.rend(); ++it) {
        if (it->type == EditOp::INSERT) {
            auto [left, rest] = split(std::move(root_), it->pos);
            auto [_, right] = split(std::move(rest), it->text.size());
            root_ = merge(std::move(left), std::move(right));
            redoGroup.ops.push_back({EditOp::ERASE, it->pos, it->text});
        } else {
            auto newNode = buildTree(it->text);
            auto [left, right] = split(std::move(root_), it->pos);
            root_ = merge(merge(std::move(left), std::move(newNode)), std::move(right));
            redoGroup.ops.push_back({EditOp::INSERT, it->pos, it->text});
        }
    }
    redoStack_.push_back(std::move(redoGroup));
    undoStack_.pop_back();
}

void Rope::redo() {
    if (redoStack_.empty()) return;
    auto& group = redoStack_.back();
    UndoGroup undoGroup;
    undoGroup.timestamp = group.timestamp;
    for (auto it = group.ops.rbegin(); it != group.ops.rend(); ++it) {
        if (it->type == EditOp::INSERT) {
            auto [left, rest] = split(std::move(root_), it->pos);
            auto [_, right] = split(std::move(rest), it->text.size());
            root_ = merge(std::move(left), std::move(right));
            undoGroup.ops.push_back({EditOp::ERASE, it->pos, it->text});
        } else {
            auto newNode = buildTree(it->text);
            auto [left, right] = split(std::move(root_), it->pos);
            root_ = merge(merge(std::move(left), std::move(newNode)), std::move(right));
            undoGroup.ops.push_back({EditOp::INSERT, it->pos, it->text});
        }
    }
    undoStack_.push_back(std::move(undoGroup));
    redoStack_.pop_back();
}

void Rope::recordInsert(size_t pos, std::string_view text) {
    auto now = std::chrono::steady_clock::now();
    if (grouping_ && !currentGroup_.ops.empty() && (now - currentGroup_.timestamp) < undoGroupWindow_) {
        currentGroup_.ops.push_back({EditOp::INSERT, pos, std::string(text)});
        return;
    }
    if (!currentGroup_.ops.empty()) {
        undoStack_.push_back(std::move(currentGroup_));
        redoStack_.clear();
    }
    currentGroup_.ops.clear();
    currentGroup_.ops.push_back({EditOp::INSERT, pos, std::string(text)});
    currentGroup_.timestamp = now;
    grouping_ = true;
}

void Rope::recordErase(size_t pos, std::string_view text) {
    auto now = std::chrono::steady_clock::now();
    if (grouping_ && !currentGroup_.ops.empty() && (now - currentGroup_.timestamp) < undoGroupWindow_) {
        currentGroup_.ops.push_back({EditOp::ERASE, pos, std::string(text)});
        return;
    }
    if (!currentGroup_.ops.empty()) {
        undoStack_.push_back(std::move(currentGroup_));
        redoStack_.clear();
    }
    currentGroup_.ops.clear();
    currentGroup_.ops.push_back({EditOp::ERASE, pos, std::string(text)});
    currentGroup_.timestamp = now;
    grouping_ = true;
}
