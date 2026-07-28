#pragma once

// Private state behind the public handles.
//
// The handle wraps this rather than wrapping the scene directly, for one
// reason: the derived data below cannot live on the scene itself. A handle owns
// a shared_ptr<const RenderScene>, so there is nowhere on the scene to cache
// anything, and the scene is shared across threads (the viewer hands one to a
// worker while the main thread still holds it).
//
// So the caches are computed once, eagerly, at wrap time. That is a few
// milliseconds on a large scene — negligible beside tessellation — and it
// removes the question of thread-safe lazy initialisation entirely rather than
// answering it.

#include <nodehammer/ir/render.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace nodehammer::detail {

struct RenderSceneState {
    std::shared_ptr<const RenderScene> scene;

    /// Ascending by id — a stable public order that does not depend on the
    /// container's iteration order.
    std::vector<MeshAssetId> meshIds;

    std::size_t triangleCount{0};

    explicit RenderSceneState(std::shared_ptr<const RenderScene> s);
};

} // namespace nodehammer::detail
