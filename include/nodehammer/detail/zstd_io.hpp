#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <zstd.h>

namespace nodehammer::zstd_io {

/// Returns true if the path ends with ".zst" (case-insensitive).
inline bool hasZstdExtension(const std::filesystem::path &path) {
    const auto ext = path.extension().string();
    return ext == ".zst" || ext == ".ZST";
}

/// Compress a string with zstd at the given level (default 3).
inline std::string compress(std::string_view input, int level = 3) {
    const std::size_t bound = ZSTD_compressBound(input.size());
    std::string output(bound, '\0');
    const std::size_t actual =
        ZSTD_compress(output.data(), output.size(), input.data(), input.size(), level);
    if (ZSTD_isError(actual)) {
        throw std::runtime_error(std::string("zstd compress failed: ") + ZSTD_getErrorName(actual));
    }
    output.resize(actual);
    return output;
}

/// Decompress a zstd-compressed buffer.
inline std::string decompress(std::string_view input) {
    const unsigned long long frameSize = ZSTD_getFrameContentSize(input.data(), input.size());
    if (frameSize == ZSTD_CONTENTSIZE_UNKNOWN || frameSize == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("zstd decompress: cannot determine frame content size");
    }
    std::string output(static_cast<std::size_t>(frameSize), '\0');
    const std::size_t actual =
        ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(actual)) {
        throw std::runtime_error(std::string("zstd decompress failed: ") +
                                 ZSTD_getErrorName(actual));
    }
    output.resize(actual);
    return output;
}

/// Read a file into a string (binary mode).
inline std::string readFile(const std::filesystem::path &path) {
    std::ifstream f{path, std::ios::binary | std::ios::ate};
    if (!f) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    const auto size = f.tellg();
    f.seekg(0);
    std::string buf(static_cast<std::size_t>(size), '\0');
    f.read(buf.data(), size);
    return buf;
}

/// Write a string to a file (binary mode).
inline void writeFile(const std::filesystem::path &path, std::string_view data) {
    std::ofstream f{path, std::ios::binary};
    if (!f) {
        throw std::runtime_error("failed to open file for writing: " + path.string());
    }
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

/// Write JSON string to a file, compressing with zstd if the path ends in .zst.
inline void writeJsonToFile(const std::filesystem::path &path, const std::string &json) {
    if (hasZstdExtension(path)) {
        writeFile(path, compress(json));
    } else {
        std::ofstream f{path};
        f << json << '\n';
    }
}

/// Read a JSON string from a file, decompressing if the path ends in .zst.
inline std::string readJsonFromFile(const std::filesystem::path &path) {
    if (hasZstdExtension(path)) {
        return decompress(readFile(path));
    }
    return readFile(path);
}

} // namespace nodehammer::zstd_io
