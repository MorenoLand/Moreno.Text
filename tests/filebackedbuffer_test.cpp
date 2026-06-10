#include "Core/FileBackedBuffer.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static std::string readAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main() {
    fs::path dir = fs::temp_directory_path() / "morenotext_filebackedbuffer_test";
    fs::create_directories(dir);
    fs::path input = dir / "input.txt";
    fs::path output = dir / "output.txt";

    {
        std::ofstream file(input, std::ios::binary | std::ios::trunc);
        file << "alpha\nbeta\ngamma\n";
    }

    FileBackedBuffer buffer;
    assert(buffer.open(input));
    assert(buffer.fileSize() == 17);
    assert(buffer.lineCount() == 4);
    assert(buffer.readOriginalLine(1) == "beta\n");

    buffer.replaceOriginalRange(buffer.lineStart(1), buffer.lineEnd(1), "BETA\n");
    assert(buffer.saveTo(output));
    assert(readAll(output) == "alpha\nBETA\ngamma\n");

    FileBackedBuffer inserted;
    assert(inserted.open(input));
    inserted.replaceOriginalRange(0, 0, "HEAD\n");
    assert(inserted.saveTo(output));
    assert(readAll(output) == "HEAD\nalpha\nbeta\ngamma\n");

    fs::remove_all(dir);
    return 0;
}
