#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace nodehammer::detail::file_io {

/// Read a file into a byte vector (binary mode).
inline std::vector<std::byte> readFile(const std::filesystem::path &path) {
    std::ifstream f{path, std::ios::binary | std::ios::ate};
    if (!f) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<std::byte> buf(size);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size));
    return buf;
}

/// Write a byte buffer to a file (binary mode).
inline void writeFile(const std::filesystem::path &path, std::span<const std::byte> data) {
    std::ofstream f{path, std::ios::binary};
    if (!f) {
        throw std::runtime_error("failed to open file for writing: " + path.string());
    }
    f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

} // namespace nodehammer::detail::file_io
