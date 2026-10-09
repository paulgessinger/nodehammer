#pragma once
#include <cstddef>
#include <filesystem>
#include <ir/semantic.hpp>
#include <span>
#include <vector>

namespace nodehammer::ir::semantic {
// NHS9: one shared FlatBuffer, optionally compressed as one zstd frame.
// No expanded tree is serialized. The FlatBuffers single-buffer size limit still
// applies to the compact definition graph; future chunking is intentionally deferred.
[[nodiscard]] std::vector<std::byte> sceneToBytes(const Scene &geometry);
// Accept NHS9 and legacy NHS8, raw or whole-file zstd. Legacy nodes remain
// authoritative through fromExpanded. Unsupported formats fail explicitly.
[[nodiscard]] Scene sceneFromBytes(std::span<const std::byte> bytes);
void writeFlatbuffer(const Scene &geometry, const std::filesystem::path &path,
                     int compressionLevel = 3);
[[nodiscard]] Scene readFlatbuffer(const std::filesystem::path &path);
} // namespace nodehammer::ir::semantic
