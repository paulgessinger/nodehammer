#pragma once

#include <ir/expanded/scene.hpp>
#include <ir/semantic/importer.hpp>

#include <string_view>
#include <vector>

namespace nodehammer::ir {

/// Builds pre-defined synthetic expanded::Scene fixtures for testing
/// and for use as the "synthetic" importer format.
class SyntheticSceneBuilder {
  public:
    /// A single box (10×10×10 mm half-lengths) with aluminum material.
    [[nodiscard]] static expanded::Scene buildSingleBox();

    /// World box containing one child box translated 100 mm along Z.
    /// Useful for testing parent-child transform accumulation.
    [[nodiscard]] static expanded::Scene buildNestedBoxes();

    /// World box with an inner tube (rMin=0, rMax=5, dz=10).
    [[nodiscard]] static expanded::Scene buildTubeInBox();

    /// World box with a BooleanSubtraction child shape.
    [[nodiscard]] static expanded::Scene buildBooleanSubtraction();

    /// Scene with one node whose shape is UnknownShape.
    /// Sets DegradationBit::UnknownShape on the node's provenance and emits
    /// a NH0102 warning into the returned DiagnosticList.
    [[nodiscard]] static expanded::ImportResult buildWithDiagnostics();
};

/// ISemanticImporter implementation for the "synthetic" format.
/// Ignores the path argument and builds a single-box scene.
class SyntheticImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;
};

} // namespace nodehammer::ir
