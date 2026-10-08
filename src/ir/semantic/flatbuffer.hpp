#pragma once
#include <cstddef>
#include <filesystem>
#include <ir/semantic.hpp>
#include <span>
#include <vector>

namespace nodehammer::ir::semantic {
// Canonical scene transport. NHS8 remains the wire format until the next stack PR.
[[nodiscard]] std::vector<std::byte> sceneToBytes(const Scene &geometry);
[[nodiscard]] Scene sceneFromBytes(std::span<const std::byte> bytes);
void writeFlatbuffer(const Scene &geometry, const std::filesystem::path &path,
                     int compressionLevel = 3);
[[nodiscard]] Scene readFlatbuffer(const std::filesystem::path &path);
} // namespace nodehammer::ir::semantic
