#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class FileBackedBuffer {
public:
    bool open(const std::filesystem::path& path);
    bool saveTo(const std::filesystem::path& path) const;

    size_t lineCount() const { return lineOffsets_.empty() ? 1 : lineOffsets_.size(); }
    uint64_t fileSize() const { return fileSize_; }
    bool empty() const { return fileSize_ == 0 && addBuffer_.empty(); }

    uint64_t lineStart(size_t line) const;
    uint64_t lineEnd(size_t line) const;
    std::string readOriginal(uint64_t offset, uint64_t length) const;
    std::string readOriginalLine(size_t line) const;

    void replaceOriginalRange(uint64_t start, uint64_t end, std::string_view text);
    bool dirty() const { return dirty_; }

private:
    enum class PieceSource { Original, Add };
    struct Piece {
        PieceSource source = PieceSource::Original;
        uint64_t start = 0;
        uint64_t length = 0;
    };

    std::filesystem::path path_;
    uint64_t fileSize_ = 0;
    std::vector<uint64_t> lineOffsets_{0};
    std::vector<Piece> pieces_;
    std::string addBuffer_;
    bool dirty_ = false;

    size_t pieceIndexAt(uint64_t logicalOffset, uint64_t& pieceLocalOffset) const;
    std::string readPiece(const Piece& piece, uint64_t offset, uint64_t length) const;
};
