#pragma once

#include <ir/semantic.hpp>
#include <tessellation/tessellator.hpp>

#include <manifold/manifold.h>

namespace nodehammer {

/// Convert a TessellationOutput mesh to a Manifold, welding duplicate vertices.
/// Returns nullopt (with diagnostics) if the mesh is not manifold-compatible.
[[nodiscard]] std::optional<manifold::Manifold>
meshToManifold(const TessellationOutput &mesh, DiagnosticList &diags, std::string_view context);

/// Convex hull of a point cloud, as a flat-shaded watertight mesh. Used to
/// build a gap-free LOD proxy for a merged sampling stack (the hull spans the
/// air gaps between slabs while following the outer silhouette). Returns an
/// empty output when the points are degenerate (< 4, or coplanar) or the hull
/// fails.
[[nodiscard]] TessellationOutput convexHull(const std::vector<glm::vec3> &points);

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
