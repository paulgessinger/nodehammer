#pragma once
#include <nodehammer/semantic_scene.hpp>
#include <span>
#include <vector>

namespace nodehammer {
/// Decode raw or zstd-compressed NHB bytes. The input is borrowed for this call.
[[nodiscard]] NH_API SemanticResult fromNhb(std::span<const std::byte> bytes);
/// Encode a scene without compression. Throws Error for an empty handle.
[[nodiscard]] NH_API std::vector<std::byte> toNhb(const SemanticScene &scene);
/// Encode a scene with zstd compression, using zstd's compression level scale.
[[nodiscard]] NH_API std::vector<std::byte> toNhbZstd(const SemanticScene &scene,
                                                      int compressionLevel = 3);
} // namespace nodehammer
