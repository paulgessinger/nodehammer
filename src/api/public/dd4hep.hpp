#pragma once
#include <nodehammer/semantic_scene.hpp>
namespace dd4hep {
class Detector;
}

namespace nodehammer {
/// Traverse caller-owned geometry. The input remains owned by the caller;
/// the returned scene owns an independent imported representation.
[[nodiscard]] NH_API SemanticResult fromDD4hep(dd4hep::Detector &detector);
} // namespace nodehammer
