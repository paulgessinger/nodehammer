#pragma once
#include <filesystem>
#include <nodehammer/config.hpp>
#include <nodehammer/import_options.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <span>
#include <string>
#include <string_view>

namespace nodehammer {
struct SemanticWriteOptions {
    /// Overrides format inference, but never overrides filename compression.
    std::string format{};
    /// Used only for a .zst output; setting this does not enable compression.
    int compressionLevel = 3;
};
struct RenderWriteOptions {
    std::string format{};
    int compressionLevel = 3;
};

[[nodiscard]] NH_API SemanticResult readSemantic(const std::filesystem::path &path,
                                                 const SemanticReadOptions &options = {});
[[nodiscard]] NH_API RenderScene readRender(const std::filesystem::path &path);
/// A .zst suffix selects compression; the underlying suffix selects the format.
/// Unsupported format/compression combinations throw Error.
NH_API void write(const SemanticScene &scene, const std::filesystem::path &path,
                  const SemanticWriteOptions &options = {});
NH_API void write(const RenderScene &scene, const std::filesystem::path &path,
                  const OutputConfig &output = {}, const RenderWriteOptions &options = {});

/// Views over library-lifetime storage. Lists reflect this build's capabilities.
[[nodiscard]] NH_API std::span<const std::string_view> semanticReadFormats();
[[nodiscard]] NH_API std::span<const std::string_view> semanticWriteFormats();
[[nodiscard]] NH_API std::span<const std::string_view> renderReadFormats();
[[nodiscard]] NH_API std::span<const std::string_view> renderWriteFormats();
} // namespace nodehammer
