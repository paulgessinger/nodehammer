#pragma once

#include <diagnostics.hpp>
#include <ir/render.hpp>
#include <ir/semantic.hpp>

#include <vector>

namespace nodehammer::tessellation {

struct TessellationParams {
    int maxSegmentsCircle{64}; ///< Segments per full circle (tubes, cones, torus, pcon, pgon)
};

struct TessellationOutput {
    std::vector<ir::render::Vertex> vertices;
    std::vector<uint32_t> indices;
    DiagnosticList diags;
    /// Whether the producing operation completed. False marks a genuine failure
    /// (e.g. a boolean whose operands could not be built). Note that `succeeded`
    /// with empty `vertices` is a *valid* result: a boolean can legitimately
    /// remove a solid entirely (e.g. a placement fully inside an angle-cut
    /// wedge), which should render nothing rather than be treated as a failure.
    bool succeeded{true};
};

/// Interface for shape tessellators.
class ITessellator {
  public:
    virtual ~ITessellator() = default;

    /// Returns true if this tessellator can produce geometry for the given shape.
    /// BooleanShape variants return false (require Manifold, Phase 2).
    [[nodiscard]] virtual bool
    canTessellate(const ir::semantic::ShapeVariant &shape) const noexcept = 0;

    /// Tessellate the shape. Called only when canTessellate() returns true.
    [[nodiscard]] virtual TessellationOutput tessellate(const ir::semantic::ShapeVariant &shape,
                                                        const TessellationParams &params) const = 0;
};

} // namespace nodehammer::tessellation
