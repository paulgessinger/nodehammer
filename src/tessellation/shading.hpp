#pragma once

#include <tessellation/tessellator.hpp>

namespace nodehammer::tessellation {

/// Rebuild corner normals without changing positions or triangle order. Smooth
/// only across manifold edges with coincident endpoints and a dihedral angle
/// at most smoothAngle degrees; disconnected fans and non-manifold edges stay sharp.
void applyShading(TessellationOutput &mesh, config::Shading shading, double smoothAngle);

} // namespace nodehammer::tessellation
