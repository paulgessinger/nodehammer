#pragma once

#include <nodehammer/detail/file_io.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <zstd.h>

namespace nodehammer::zstd_io {

/// Returns true if the path ends with ".zst" (case-insensitive).
inline bool hasZstdExtension(const std::filesystem::path &path) {
    const auto ext = path.extension().string();
    return ext == ".zst" || ext == ".ZST";
}

/// Compress a byte buffer with zstd at the given level (default 3).
inline std::vector<std::byte> compress(std::span<const std::byte> input, int level = 3) {
    const std::size_t bound = ZSTD_compressBound(input.size());
    std::vector<std::byte> output(bound);
    const std::size_t actual =
        ZSTD_compress(output.data(), output.size(), input.data(), input.size(), level);
    if (ZSTD_isError(actual) != 0u) {
        throw std::runtime_error(std::string("zstd compress failed: ") + ZSTD_getErrorName(actual));
    }
    output.resize(actual);
    return output;
}

/// Decompress a zstd-compressed byte buffer.
inline std::vector<std::byte> decompress(std::span<const std::byte> input) {
    const unsigned long long frameSize = ZSTD_getFrameContentSize(input.data(), input.size());
    if (frameSize == ZSTD_CONTENTSIZE_UNKNOWN || frameSize == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("zstd decompress: cannot determine frame content size");
    }
    std::vector<std::byte> output(static_cast<std::size_t>(frameSize));
    const std::size_t actual =
        ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(actual) != 0u) {
        throw std::runtime_error(std::string("zstd decompress failed: ") +
                                 ZSTD_getErrorName(actual));
    }
    output.resize(actual);
    return output;
}

/// Write JSON string to a file, compressing with zstd if the path ends in .zst.
inline void writeJsonToFile(const std::filesystem::path &path, const std::string &json) {
    auto asBytes = std::as_bytes(std::span{json});
    if (hasZstdExtension(path)) {
        auto compressed = compress(asBytes);
        file_io::writeFile(path, compressed);
    } else {
        // Plain text: append newline
        std::ofstream f{path};
        f << json << '\n';
    }
}

/// Read a JSON string from a file, decompressing if the path ends in .zst.
inline std::string readJsonFromFile(const std::filesystem::path &path) {
    auto raw = file_io::readFile(path);
    if (hasZstdExtension(path)) {
        auto decompressed = decompress(raw);
        return {reinterpret_cast<const char *>(decompressed.data()), decompressed.size()};
    }
    return {reinterpret_cast<const char *>(raw.data()), raw.size()};
}

} // namespace nodehammer::zstd_io
