#include "ao_denoise_pass.hpp"

#include <nodehammer/viewer/backend_caps.hpp>
#include <nodehammer/viewer/camera.hpp>

#include "ao_render_target.hpp"
#include "scene_render_target.hpp"

#include <algorithm>
#include <cstring>

#include "ao_denoise.glsl.h"

namespace nodehammer::viewer {

AoDenoisePass::AoDenoisePass() = default;
AoDenoisePass::~AoDenoisePass() = default;

void AoDenoisePass::initialize() {
    if (initialized_) {
        return;
    }
    shader_ = sg_make_shader(ao_denoise_ao_denoise_shader_desc(sg_query_backend()));

    // Fullscreen triangle in NDC. Bound at vertex_buffers[0] so attribute 0
    // has an enabled array — WebGL2 otherwise warns about expensive
    // emulation (see ao_pass.cpp / composite_pass.cpp for the long form).
    static const float fullscreen_tri[6] = {
        -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f,
    };
    sg_buffer_desc bdesc{};
    bdesc.usage.vertex_buffer = true;
    bdesc.data = SG_RANGE(fullscreen_tri);
    bdesc.label = "ao_denoise_fullscreen_tri";
    vbuf_ = sg_make_buffer(&bdesc);

    initialized_ = true;
}

void AoDenoisePass::setTargetColorFormat(sg_pixel_format fmt) {
    if (!initialized_) {
        initialize();
    }
    if (pipeline_.id != SG_INVALID_ID && current_color_format_ == fmt) {
        return;
    }
    if (pipeline_.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_);
        pipeline_ = sg_pipeline{};
    }

    sg_pipeline_desc pdesc{};
    pdesc.shader = shader_;
    pdesc.layout.buffers[0].stride = static_cast<int>(sizeof(float) * 2);
    pdesc.layout.attrs[ATTR_ao_denoise_ao_denoise_a_pos].buffer_index = 0;
    pdesc.layout.attrs[ATTR_ao_denoise_ao_denoise_a_pos].format = SG_VERTEXFORMAT_FLOAT2;
    pdesc.layout.attrs[ATTR_ao_denoise_ao_denoise_a_pos].offset = 0;
    pdesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pdesc.cull_mode = SG_CULLMODE_NONE;
    pdesc.depth.write_enabled = false;
    pdesc.depth.compare = SG_COMPAREFUNC_ALWAYS;
    // The denoise pass renders into a color-only attachment, same as the
    // raw AO pass — pin depth format to NONE explicitly so WebGPU doesn't
    // reject the depthless render pass against a DEPTH_STENCIL default.
    pdesc.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pdesc.color_count = 1;
    pdesc.colors[0].pixel_format = fmt;
    pdesc.label = "ao_denoise_pipeline";
    pipeline_ = sg_make_pipeline(&pdesc);
    current_color_format_ = fmt;
}

void AoDenoisePass::release() {
    if (!initialized_) {
        return;
    }
    if (pipeline_.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_);
        pipeline_ = sg_pipeline{};
    }
    current_color_format_ = SG_PIXELFORMAT_NONE;
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

void AoDenoisePass::draw(const AoRenderTarget &raw_ao, const SceneRenderTarget &scene_rt,
                         const Camera &camera, uint32_t target_w, uint32_t target_h) {
    if (!initialized_ || pipeline_.id == SG_INVALID_ID || target_w == 0 || target_h == 0 ||
        raw_ao.color.id == SG_INVALID_ID || scene_rt.depth.id == SG_INVALID_ID) {
        return;
    }

    sg_apply_pipeline(pipeline_);

    sg_bindings bind{};
    bind.vertex_buffers[0] = vbuf_;
    bind.views[VIEW_ao_denoise_ao_raw] = raw_ao.color_texture_view;
    bind.views[VIEW_ao_denoise_scene_depth] = scene_rt.depth_texture_view;
    bind.samplers[SMP_ao_denoise_smp_ao] = raw_ao.sampler;
    bind.samplers[SMP_ao_denoise_smp_depth] = scene_rt.depth_sampler;
    sg_apply_bindings(&bind);

    ao_denoise_ao_denoise_params_block_t params{};

    // Depth-mode convention mirrors composite.glsl and ao.glsl (one source
    // of truth, three samplers). useLogDepth() takes priority on GLES3.
    const float depth_mode = useLogDepth() ? 2.0f : (useReversedZ() ? 1.0f : 0.0f);
    params.depth_params[0] = depth_mode;
    params.depth_params[1] = camera.far_plane;
    params.depth_params[2] = camera.near_plane;
    // sigma_z controls how aggressively the bilateral filter rejects taps
    // across a depth discontinuity. ~1% of the depth range keeps the filter
    // tight on silhouettes (no AO bleed across object boundaries) while
    // still allowing the 3×3 kernel to smooth uniform-depth regions. The
    // depth-range fallback handles the degenerate case of near≈far where
    // a tiny sigma would zero out the whole kernel.
    const float depth_range = std::max(camera.far_plane - camera.near_plane, 1e-3f);
    params.depth_params[3] = 0.01f * depth_range;

    const float inv_w = 1.0f / static_cast<float>(target_w);
    const float inv_h = 1.0f / static_cast<float>(target_h);
    params.frame_params[0] = inv_w;
    params.frame_params[1] = inv_h;
    params.frame_params[2] = sg_query_features().origin_top_left ? 1.0f : 0.0f;
    params.frame_params[3] = 0.0f; // reserved

    sg_range u{&params, sizeof(params)};
    sg_apply_uniforms(UB_ao_denoise_ao_denoise_params_block, &u);

    sg_draw(0, 3, 1);
}

} // namespace nodehammer::viewer
