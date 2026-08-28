#pragma once

// The `RenderScene` handle's `Impl`, and the helpers that build and read one.
//
// Split out of handles.hpp so a TU that does not touch the render IR does not
// pay for `ir/render.hpp` -- which reaches glm. See the note there.

#include <api/handles.hpp>

#include <ir/render.hpp>
#include <nodehammer/render_scene.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace nodehammer {

struct RenderScene::Impl {
    ir::render::Scene scene;
};

inline RenderScene::RenderScene(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const RenderScene::Impl &RenderScene::impl() const {
    if (!impl_) {
        throw Error{codes::kFatalApiInvalidHandle, "the render scene handle refers to nothing"};
    }
    return *impl_;
}

} // namespace nodehammer

namespace nodehammer::api {

[[nodiscard]] inline RenderScene asHandle(ir::render::Scene scene) {
    return RenderScene{
        std::make_shared<const RenderScene::Impl>(RenderScene::Impl{std::move(scene)})};
}

[[nodiscard]] inline const ir::render::Scene &sceneOrThrow(const RenderScene &handle,
                                                           std::string_view verb) {
    if (!handle.valid()) {
        throw Error{codes::kFatalApiInvalidHandle, "the render scene handle refers to nothing",
                    verb};
    }
    return handle.impl().scene;
}

} // namespace nodehammer::api
