#pragma once

#include <ir/render/exporter.hpp>

namespace nodehammer::ir {

/// Writes a render::Scene to glTF 2.0 (`.gltf`) or binary GLB (`.glb`).
///
/// Geometry is stored with interleaved POSITION+NORMAL attributes.
/// MeshAssets are deduplicated: the same asset referenced by multiple nodes
/// produces one set of buffer views and accessors, shared by all referencing nodes.
class GltfExporter final : public IRenderExporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] ExportResult write(const render::Scene &scene, const std::filesystem::path &path,
                                     const ExportConfig &config) const override;
};

} // namespace nodehammer::ir
