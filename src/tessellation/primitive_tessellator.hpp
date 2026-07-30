#pragma once

#include <tessellation/tessellator.hpp>

namespace nodehammer::tessellation {

/// Tessellates primitive shapes (Box, Tube, Cone, Trd, Para, Torus, Pcon, Pgon,
/// TessellatedShape). Returns false from canTessellate() for Boolean variants.
/// UnknownShape produces an empty mesh and an NH0500 error diagnostic.
class PrimitiveTessellator final : public ITessellator {
  public:
    [[nodiscard]] bool
    canTessellate(const ir::semantic::ShapeVariant &shape) const noexcept override;
    [[nodiscard]] TessellationOutput tessellate(const ir::semantic::ShapeVariant &shape,
                                                const TessellationParams &params) const override;
};

} // namespace nodehammer::tessellation
