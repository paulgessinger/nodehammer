#pragma once

// Freeze points: convert between the internal IR and the public handles.
// **Not public API.**
//
// A consumer never calls these — it receives handles from the pipeline verbs
// (`importGeometry`, `buildScene`) and passes them along. Only code that has
// built an internal scene and wants to hand out a handle needs them.
//
// Keeping them here rather than in <nodehammer/scene.hpp> is what lets the
// public headers get away with forward declarations. `wrapSemanticScene` takes
// the internal scene *by value*, so its callers need the complete type; if it
// were declared publicly, installing the public headers alone would not be
// enough to use them.

#include <nodehammer/render.hpp>
#include <nodehammer/scene.hpp>

#include <memory>

namespace nodehammer::detail {

class SemanticScene;
struct RenderScene;

/// Freeze a scene into a handle. Takes shared ownership of an already-const
/// scene, so no mutable alias to it can survive the call. Eagerly computes the
/// handle's cached orders and stats.
[[nodiscard]] nodehammer::SemanticScene
wrapSemanticScene(std::shared_ptr<const SemanticScene> scene);

/// Convenience for producers holding a scene by value, which is how every
/// importer builds one. Freezes by move.
[[nodiscard]] nodehammer::SemanticScene wrapSemanticScene(SemanticScene scene);

[[nodiscard]] const std::shared_ptr<const SemanticScene> &
unwrapSemanticScene(const nodehammer::SemanticScene &handle) noexcept;

[[nodiscard]] nodehammer::RenderScene wrapRenderScene(std::shared_ptr<const RenderScene> scene);

[[nodiscard]] const std::shared_ptr<const RenderScene> &
unwrapRenderScene(const nodehammer::RenderScene &handle) noexcept;

} // namespace nodehammer::detail
