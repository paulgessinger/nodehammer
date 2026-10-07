#pragma once
#include <nodehammer/semantic_scene.hpp>
class TGeoManager;

namespace nodehammer {
/// Traverse caller-owned geometry. The input remains owned by the caller;
/// the returned scene owns an independent imported representation.
[[nodiscard]] NH_API SemanticResult fromTGeo(TGeoManager &manager);
} // namespace nodehammer
