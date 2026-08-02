#pragma once

#include <ir/render/exporter.hpp>

namespace nodehammer::ir {

/// Writes a render::Scene to Wavefront OBJ + MTL.
///
/// Vertex positions are transformed to world space using each node's worldTransform.
/// Normals are transformed with the inverse-transpose of the worldTransform.
/// A companion .mtl file is written alongside the .obj with Kd/Ks/Ke entries
/// derived from each material's PBR baseColorFactor and emissiveFactor.
class ObjExporter final : public IRenderExporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    void write(const render::Scene &scene, const std::filesystem::path &path,
               const ExportConfig &config) const override;
};

} // namespace nodehammer::ir
