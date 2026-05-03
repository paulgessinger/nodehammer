#include "composite_pass.hpp"

#include "scene_render_target.hpp"

// sokol-shdc emits a reflection helper that calls strcmp; pull in <cstring>
// before the generated header so the TU compiles standalone.
#include <cstring>

#include "composite.glsl.h"

namespace nodehammer::viewer {

CompositePass::CompositePass() = default;

CompositePass::~CompositePass() = default;

void CompositePass::initialize() {
    if (initialized_) {
        return;
    }
    shader_ = sg_make_shader(composite_composite_shader_desc(sg_query_backend()));

    sg_pipeline_desc pdesc{};
    pdesc.shader = shader_;
    // No vertex buffers — VS reads gl_VertexIndex.
    pdesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pdesc.cull_mode = SG_CULLMODE_NONE;
    // Composite writes color only; the swapchain pass owns its own depth
    // attachment and we don't write into it.
    pdesc.depth.write_enabled = false;
    pdesc.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pdesc.color_count = 1;
    pdesc.label = "composite_pipeline";
    pipeline_ = sg_make_pipeline(&pdesc);

    initialized_ = true;
}

void CompositePass::release() {
    if (!initialized_) {
        return;
    }
    if (pipeline_.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_);
        pipeline_ = sg_pipeline{};
    }
    if (shader_.id != SG_INVALID_ID) {
        sg_destroy_shader(shader_);
        shader_ = sg_shader{};
    }
    initialized_ = false;
}

void CompositePass::draw(const SceneRenderTarget &target, DebugView mode, float near_plane,
                         float far_plane) {
    if (!initialized_ || target.color.id == SG_INVALID_ID) {
        return;
    }

    sg_apply_pipeline(pipeline_);

    sg_bindings bind{};
    bind.views[VIEW_composite_scene_color] = target.color_texture_view;
    bind.views[VIEW_composite_scene_depth] = target.depth_texture_view;
    bind.samplers[SMP_composite_smp] = target.sampler;
    sg_apply_bindings(&bind);

    composite_composite_params_t params{};
    params.mode_near_far[0] = static_cast<float>(static_cast<int>(mode));
    params.mode_near_far[1] = near_plane;
    params.mode_near_far[2] = far_plane;
    params.mode_near_far[3] = 0.0f;
    sg_range u{&params, sizeof(params)};
    sg_apply_uniforms(UB_composite_composite_params, &u);

    sg_draw(0, 3, 1);
}

} // namespace nodehammer::viewer
