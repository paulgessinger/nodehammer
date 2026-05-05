#include "composite_pass.hpp"

#include <nodehammer/viewer/backend_caps.hpp>

#include "ao_pass.hpp"
#include "ao_render_target.hpp"
#include "scene_render_target.hpp"

// sokol-shdc emits a reflection helper that calls strcmp; pull in <cstring>
// before the generated header so the TU compiles standalone.
#include <cmath>
#include <cstring>

#include <glm/gtc/type_ptr.hpp>

#include "composite.glsl.h"

namespace nodehammer::viewer {

CompositePass::CompositePass() = default;

CompositePass::~CompositePass() = default;

void CompositePass::initialize() {
    if (initialized_) {
        return;
    }
    shader_ = sg_make_shader(composite_composite_shader_desc(sg_query_backend()));

    // Fullscreen triangle in NDC. Bound at vertex_buffers[0] so attribute 0
    // has an enabled array — WebGL2 otherwise warns about expensive emulation.
    static const float fullscreen_tri[6] = {
        -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f,
    };
    sg_buffer_desc bdesc{};
    bdesc.usage.vertex_buffer = true;
    bdesc.data = SG_RANGE(fullscreen_tri);
    bdesc.label = "composite_fullscreen_tri";
    vbuf_ = sg_make_buffer(&bdesc);

    sg_pipeline_desc pdesc{};
    pdesc.shader = shader_;
    pdesc.layout.buffers[0].stride = static_cast<int>(sizeof(float) * 2);
    pdesc.layout.attrs[ATTR_composite_composite_a_pos].buffer_index = 0;
    pdesc.layout.attrs[ATTR_composite_composite_a_pos].format = SG_VERTEXFORMAT_FLOAT2;
    pdesc.layout.attrs[ATTR_composite_composite_a_pos].offset = 0;
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
    if (vbuf_.id != SG_INVALID_ID) {
        sg_destroy_buffer(vbuf_);
        vbuf_ = sg_buffer{};
    }
    if (shader_.id != SG_INVALID_ID) {
        sg_destroy_shader(shader_);
        shader_ = sg_shader{};
    }
    initialized_ = false;
}

void CompositePass::draw(const SceneRenderTarget &scene_target, const AoRenderTarget &ao_target,
                         const AoPass &ao_pass, const RenderQualitySettings &quality,
                         float near_plane, float far_plane, sg_view background_env,
                         sg_sampler env_sampler, const glm::mat4 &inv_view_proj,
                         const glm::vec3 &camera_pos) {
    if (!initialized_ || scene_target.color.id == SG_INVALID_ID) {
        return;
    }

    sg_apply_pipeline(pipeline_);

    // AO binding: real AO target view+sampler when AO is on, AoPass's 1×1
    // white dummy otherwise. Keeping the binding always-valid avoids a
    // second composite pipeline variant.
    const bool ao_on = quality.enable_ao && ao_target.color.id != SG_INVALID_ID;
    const sg_view ao_view = ao_on ? ao_target.color_texture_view : ao_pass.dummyView();
    const sg_sampler ao_sampler = ao_on ? ao_target.sampler : ao_pass.dummySampler();

    sg_bindings bind{};
    bind.vertex_buffers[0] = vbuf_;
    bind.views[VIEW_composite_scene_color] = scene_target.color_texture_view;
    bind.views[VIEW_composite_scene_depth] = scene_target.depth_texture_view;
    bind.views[VIEW_composite_ao_map] = ao_view;
    bind.views[VIEW_composite_background_env] = background_env;
    bind.samplers[SMP_composite_smp_color] = scene_target.sampler;
    bind.samplers[SMP_composite_smp_depth] = scene_target.depth_sampler;
    bind.samplers[SMP_composite_smp_ao] = ao_sampler;
    bind.samplers[SMP_composite_smp_env] = env_sampler;
    sg_apply_bindings(&bind);

    composite_composite_params_t params{};
    params.mode_near_far[0] = static_cast<float>(static_cast<int>(quality.debug_view));
    params.mode_near_far[1] = near_plane;
    params.mode_near_far[2] = far_plane;
    // Flip V on top-left-origin backends (Metal/D3D/WGPU). GL backends
    // (GLCore/GLES3) share the texture's bottom-left origin with the
    // framebuffer NDC y, so the sampled image is already right-side up.
    params.mode_near_far[3] = sg_query_features().origin_top_left ? 1.0f : 0.0f;

    const float inv_w =
        (scene_target.width > 0) ? 1.0f / static_cast<float>(scene_target.width) : 0.0f;
    const float inv_h =
        (scene_target.height > 0) ? 1.0f / static_cast<float>(scene_target.height) : 0.0f;
    params.fxaa_params[0] = quality.enable_fxaa ? 1.0f : 0.0f;
    params.fxaa_params[1] = inv_w;
    params.fxaa_params[2] = inv_h;
    params.fxaa_params[3] = 0.0f;

    // Selects the linearization branch in the FS for the linear-depth view.
    // 0 = normal-Z, 1 = reversed-Z, 2 = log-Z (mutually exclusive). Must
    // agree with the projection / pipeline / pass-action / scene-VS encoding.
    // .y carries far_plane for the log-Z pow inversion.
    const float depth_mode = useLogDepth() ? 2.0f : (useReversedZ() ? 1.0f : 0.0f);
    params.depth_params[0] = depth_mode;
    params.depth_params[1] = far_plane;
    params.depth_params[2] = 0.0f;
    params.depth_params[3] = 0.0f;

    // Exposure is applied unconditionally; tonemap is gated by the bool and
    // routed by the mode index in the shader (-1 = passthrough). Depth
    // debug views ignore both.
    const float exposure = std::exp2f(quality.exposure_stops);
    float tm_mode = -1.0f;
    if (quality.enable_tonemap) {
        switch (quality.tonemap_mode) {
        case TonemapMode::ACES:
            tm_mode = 0.0f;
            break;
        case TonemapMode::Reinhard:
            tm_mode = 1.0f;
            break;
        case TonemapMode::AgX:
            tm_mode = 2.0f;
            break;
        }
    }
    params.tonemap_params[0] = exposure;
    params.tonemap_params[1] = tm_mode;
    params.tonemap_params[2] = 0.0f;
    params.tonemap_params[3] = 0.0f;

    params.ao_params[0] = ao_on ? 1.0f : 0.0f;
    params.ao_params[1] = 0.0f;
    params.ao_params[2] = 0.0f;
    params.ao_params[3] = 0.0f;

    params.background_params[0] = quality.enable_background ? 1.0f : 0.0f;
    params.background_params[1] = 0.0f;
    params.background_params[2] = 0.0f;
    params.background_params[3] = 0.0f;
    std::memcpy(params.inv_view_proj, glm::value_ptr(inv_view_proj), sizeof(params.inv_view_proj));
    params.camera_pos[0] = camera_pos.x;
    params.camera_pos[1] = camera_pos.y;
    params.camera_pos[2] = camera_pos.z;
    params.camera_pos[3] = 0.0f;

    sg_range u{&params, sizeof(params)};
    sg_apply_uniforms(UB_composite_composite_params, &u);

    sg_draw(0, 3, 1);
}

} // namespace nodehammer::viewer
