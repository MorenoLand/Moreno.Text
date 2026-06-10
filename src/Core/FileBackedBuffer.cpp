#include "Core/FileBackedBuffer.h"

#include <algorithm>
#include <fstream>
#include <vector>

bool FileBackedBuffer::open(const std::filesystem::path& path) {
    path_ = path;
    fileSize_ = 0;
    lineOffsets_.clear();
    lineOffsets_.push_back(0);
    pieces_.clear();
    addBuffer_.clear();
    dirty_ = false;

    std::ifstream file(path_, std::ios::binary);
    if (!file) return false;
    try {
        fileSize_ = static_cast<uint64_t>(std::filesystem::file_size(path_));
    } catch (...) {
        return false;
    }

    std::vector<char> buffer(1024 * 1024);
    uint64_t absolute = 0;
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        std::streamsize got = file.gcount();
        if (got <= 0) break;
        for (std::streamsize i = 0; i < got; ++i) {
            if (buffer[static_cast<size_t>(i)] == '\n') lineOffsets_.push_back(absolute + static_cast<uint64_t>(i) + 1);
        }
        absolute += static_cast<uint64_t>(got);
    }
    pieces_.push_back({PieceSource::Original, 0, fileSize_});
    return true;
}

uint64_t FileBackedBuffer::lineStart(size_t line) const {
    if (line >= lineOffsets_.size()) return fileSize_;
    return lineOffsets_[line];
}

uint64_t FileBackedBuffer::lineEnd(size_t line) const {
    if (line + 1 < lineOffsets_.size()) return lineOffsets_[line + 1];
    return fileSize_;
}

std::string FileBackedBuffer::readOriginal(uint64_t offset, uint64_t length) const {
    if (offset >= fileSize_ || length == 0) return {};
    length = std::min<uint64_t>(length, fileSize_ - offset);
    std::string out(static_cast<size_t>(length), '\0');
    std::ifstream file(path_, std::ios::binary);
    if (!file) return {};
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(file.gcount()));
    return out;
}

std::string FileBackedBuffer::readOriginalLine(size_t line) const {
    uint64_t start = lineStart(line);
    uint64_t end = lineEnd(line);
    if (end < start) end = start;
    return readOriginal(start, end - start);
}

size_t FileBackedBuffer::pieceIndexAt(uint64_t logicalOffset, uint64_t& pieceLocalOffset) const {
    uint64_t walked = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        const auto& piece = pieces_[i];
        if (logicalOffset <= walked + piece.length) {
            pieceLocalOffset = logicalOffset - walked;
            return i;
        }
        walked += piece.length;
    }
    pieceLocalOffset = pieces_.empty() ? 0 : pieces_.back().length;
    return pieces_.empty() ? 0 : pieces_.size() - 1;
}

std::string FileBackedBuffer::readPiece(const Piece& piece, uint64_t offset, uint64_t length) const {
    if (offset >= piece.length || length == 0) return {};
    length = std::min<uint64_t>(length, piece.length - offset);
    if (piece.source == PieceSource::Add) {
        uint64_t start = piece.start + offset;
        if (start >= addBuffer_.size()) return {};
        length = std::min<uint64_t>(length, static_cast<uint64_t>(addBuffer_.size()) - start);
        return addBuffer_.substr(static_cast<size_t>(start), static_cast<size_t>(length));
    }
    return readOriginal(piece.start + offset, length);
}

void FileBackedBuffer::replaceOriginalRange(uint64_t start, uint64_t end, std::string_view text) {
    if (end < start) end = start;
    start = std::min<uint64_t>(start, fileSize_);
    end = std::min<uint64_t>(end, fileSize_);

    std::vector<Piece> next;
    next.reserve(pieces_.size() + 2);
    uint64_t logical = 0;
    bool inserted = false;
    for (const auto& piece : pieces_) {
        uint64_t pieceStart = logical;
        uint64_t pieceEnd = logical + piece.length;
        if (pieceEnd <= start || pieceStart >= end) {
            if (!inserted && pieceStart >= end && !text.empty()) {
                uint64_t addStart = static_cast<uint64_t>(addBuffer_.size());
                addBuffer_.append(text);
                next.push_back({PieceSource::Add, addStart, static_cast<uint64_t>(text.size())});
                inserted = true;
            }
            next.push_back(piece);
        } else {
            if (start > pieceStart) {
                next.push_back({piece.source, piece.start, start - pieceStart});
            }
            if (!inserted && !text.empty()) {
                uint64_t addStart = static_cast<uint64_t>(addBuffer_.size());
                addBuffer_.append(text);
                next.push_back({PieceSource::Add, addStart, static_cast<uint64_t>(text.size())});
                inserted = true;
            }
            if (end < pieceEnd) {
                uint64_t skip = end - pieceStart;
                next.push_back({piece.source, piece.start + skip, pieceEnd - end});
            }
        }
        logical = pieceEnd;
    }
    if (!inserted && !text.empty()) {
        uint64_t addStart = static_cast<uint64_t>(addBuffer_.size());
        addBuffer_.append(text);
        next.push_back({PieceSource::Add, addStart, static_cast<uint64_t>(text.size())});
    }
    pieces_ = std::move(next);
    dirty_ = true;
}

bool FileBackedBuffer::saveTo(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (const auto& piece : pieces_) {
        uint64_t remaining = piece.length;
        uint64_t offset = 0;
        while (remaining > 0) {
            uint64_t chunk = std::min<uint64_t>(remaining, 1024 * 1024);
            std::string data = readPiece(piece, offset, chunk);
            if (data.empty() && chunk > 0) return false;
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            offset += static_cast<uint64_t>(data.size());
            remaining -= static_cast<uint64_t>(data.size());
        }
    }
    return true;
}
