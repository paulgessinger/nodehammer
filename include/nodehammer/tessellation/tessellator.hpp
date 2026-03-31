#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <vector>

namespace nodehammer {

struct TessellationParams {
    int maxSegmentsCircle{64}; ///< Segments per full circle (tubes, cones, torus, pcon, pgon)
};

struct TessellationOutput {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    DiagnosticList diags;
};

/// Interface for shape tessellators.
class ITessellator {
  public:
    virtual ~ITessellator() = default;

    /// Returns true if this tessellator can produce geometry for the given shape.
    /// BooleanShape variants return false (require Manifold, Phase 2).
    [[nodiscard]] virtual bool canTessellate(const SemanticShapeVariant &shape) const noexcept = 0;

    /// Tessellate the shape. Called only when canTessellate() returns true.
    [[nodiscard]] virtual TessellationOutput tessellate(const SemanticShapeVariant &shape,
                                                        const TessellationParams &params) const = 0;
};

} // namespace nodehammer
