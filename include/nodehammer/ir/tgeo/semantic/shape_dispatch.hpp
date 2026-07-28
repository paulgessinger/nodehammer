#pragma once

#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>

class TGeoShape;

namespace nodehammer {

/// Dispatch a TGeoShape* to the matching SemanticShapeVariant, register the
/// resulting SemanticShape in the scene, and return its ID.
///
/// On unknown shape types emits NH0301, sets DegradationBit::UnknownShape on
/// the returned shape's provenance (via UnknownShape.originalType), and still
/// returns a valid ID — nodes are never silently omitted.
[[nodiscard]] SemanticShapeId
dispatchTGeoShape(const TGeoShape *shape, detail::SemanticScene &scene, DiagnosticList &diags);

} // namespace nodehammer
