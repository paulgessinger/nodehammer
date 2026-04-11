#pragma once

#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/tessellation/tessellator.hpp>

namespace nodehammer {

/// Recursively tessellate a boolean shape using the Manifold library.
///
/// Resolves operand shapes from the scene. For primitives, prefers Manifold's
/// built-in constructors (Box, Cylinder) for guaranteed watertight input; falls
/// back to the provided tessellator for shapes Manifold doesn't natively support.
/// Performs the boolean operation and returns the resulting mesh.
///
/// Returns an empty TessellationOutput (with diagnostics) on failure.
[[nodiscard]] TessellationOutput tessellateBooleanShape(const SemanticShapeVariant &shape,
                                                        const SemanticScene &scene,
                                                        const ITessellator &primitiveTessellator,
                                                        const TessellationParams &params);

} // namespace nodehammer
