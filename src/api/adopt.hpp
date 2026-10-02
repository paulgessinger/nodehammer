#pragma once

// Turning an internal `ImportResult` into the pair of handles the API returns.
//
// Its own header because the two halves of `SemanticScene`'s implementation
// both need it: the direct entry points in semantic_scene.cpp and the
// registry-dispatched ones in semantic_scene_formats.cpp. Anonymous-namespace
// copies in each would be two definitions free to drift, and would collide
// outright in a build that concatenates both into one translation unit.

#include <api/handles.hpp>
#include <ir/semantic/importer.hpp>

#include <utility>

namespace nodehammer::api {

/// Adopt an internal ImportResult.
///
/// Nothing to translate: an importer that could not read its input throws, so
/// what arrives here is a scene and what was observed about it. The wrapper is
/// two conversions.
[[nodiscard]] inline SemanticResult adopt(ir::ImportResult result) {
    return SemanticResult{asHandle(std::move(result.scene)), std::move(result.diags)};
}

} // namespace nodehammer::api
