#pragma once
#include <nodehammer/render_scene.hpp>
#include <span>
#include <vector>

namespace nodehammer {
/// Decode raw or zstd-compressed NHR bytes.
[[nodiscard]] NH_API RenderScene fromNhr(std::span<const std::byte> bytes);
[[nodiscard]] NH_API std::vector<std::byte> toNhr(const RenderScene &scene);
} // namespace nodehammer
