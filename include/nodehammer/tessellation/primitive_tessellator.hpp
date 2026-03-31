#pragma once

#include <nodehammer/tessellation/tessellator.hpp>

namespace nodehammer {

/// Tessellates primitive shapes (Box, Tube, Cone, Trd, Para, Torus, Pcon, Pgon,
/// TessellatedShape). Returns false from canTessellate() for Boolean variants.
/// UnknownShape produces an empty mesh and an NH0500 error diagnostic.
class PrimitiveTessellator final : public ITessellator {
  public:
    [[nodiscard]] bool canTessellate(const SemanticShapeVariant &shape) const noexcept override;
    [[nodiscard]] TessellationOutput tessellate(const SemanticShapeVariant &shape,
                                                const TessellationParams &params) const override;
};

} // namespace nodehammer
