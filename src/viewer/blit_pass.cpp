#include "blit_pass.hpp"

// sokol-shdc emits a reflection helper that calls strcmp; pull in <cstring>
// before the generated header so the TU compiles standalone.
#include <cstring>

#include "blit.glsl.h"

namespace nodehammer::viewer {

BlitPass::BlitPass() = default;
BlitPass::~BlitPass() = default;

void BlitPass::initialize() {
    if (initialized_) {
        return;
    }
    shader_ = sg_make_shader(blit_blit_shader_desc(sg_query_backend()));

    // Fullscreen triangle in NDC. Bound at vertex_buffers[0] so attribute 0 has
    // an enabled array — WebGL2 otherwise warns about expensive emulation (see
    // composite_pass.cpp for the long form).
    static const float fullscreen_tri[6] = {
        -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f,
    };
    sg_buffer_desc bdesc{};
    bdesc.usage.vertex_buffer = true;
    bdesc.data = SG_RANGE(fullscreen_tri);
    bdesc.label = "blit_fullscreen_tri";
    vbuf_ = sg_make_buffer(&bdesc);

    // NEAREST so the 1:1 copy is exact (no half-texel interpolation).
    sg_sampler_desc sdesc{};
    sdesc.min_filter = SG_FILTER_NEAREST;
    sdesc.mag_filter = SG_FILTER_NEAREST;
    sdesc.mipmap_filter = SG_FILTER_NEAREST;
    sdesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.label = "blit_sampler";
    sampler_ = sg_make_sampler(&sdesc);

    // Targets the swapchain defaults (color/depth format + sample count), like
    // CompositePass — no explicit pixel_format so the env defaults apply.
    sg_pipeline_desc pdesc{};
    pdesc.shader = shader_;
    pdesc.layout.buffers[0].stride = static_cast<int>(sizeof(float) * 2);
    pdesc.layout.attrs[ATTR_blit_blit_a_pos].buffer_index = 0;
    pdesc.layout.attrs[ATTR_blit_blit_a_pos].format = SG_VERTEXFORMAT_FLOAT2;
    pdesc.layout.attrs[ATTR_blit_blit_a_pos].offset = 0;
    pdesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pdesc.cull_mode = SG_CULLMODE_NONE;
    pdesc.depth.write_enabled = false;
    pdesc.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pdesc.color_count = 1;
    pdesc.label = "blit_pipeline";
    pipeline_ = sg_make_pipeline(&pdesc);

    initialized_ = true;
}

void BlitPass::release() {
    if (!initialized_) {
        return;
    }
    if (pipeline_.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_);
        pipeline_ = sg_pipeline{};
    }
    if (sampler_.id != SG_INVALID_ID) {
        sg_destroy_sampler(sampler_);
        sampler_ = sg_sampler{};
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

void BlitPass::draw(sg_view src_view) {
    if (!initialized_ || pipeline_.id == SG_INVALID_ID || src_view.id == SG_INVALID_ID) {
        return;
    }

    sg_apply_pipeline(pipeline_);

    sg_bindings bind{};
    bind.vertex_buffers[0] = vbuf_;
    bind.views[VIEW_blit_src] = src_view;
    bind.samplers[SMP_blit_smp] = sampler_;
    sg_apply_bindings(&bind);

    blit_blit_params_t params{};
    // Flip V on top-left-origin backends (Metal/D3D/WGPU) — the present-cache is
    // an offscreen texture, sampled into the swapchain exactly like the composite
    // samples the offscreen scene target.
    params.params[0] = sg_query_features().origin_top_left ? 1.0f : 0.0f;
    sg_range u{&params, sizeof(params)};
    sg_apply_uniforms(UB_blit_blit_params, &u);

    sg_draw(0, 3, 1);
}

} // namespace nodehammer::viewer
