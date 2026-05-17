#include "ao_pass.hpp"

#include <nodehammer/viewer/backend_caps.hpp>
#include <nodehammer/viewer/camera.hpp>

#include "scene_render_target.hpp"

#include <cmath>
#include <cstring>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "ao.glsl.h"

namespace nodehammer::viewer {

AoPass::AoPass() = default;
AoPass::~AoPass() = default;

void AoPass::initialize() {
    if (initialized_) {
        return;
    }
    shader_ = sg_make_shader(ao_ao_shader_desc(sg_query_backend()));
    // Pipeline is deferred to setTargetColorFormat — sokol pins the color
    // attachment format on a pipeline, so we can't build it until the App
    // side has picked R16F vs R8.

    // Fullscreen triangle in NDC. Bound at vertex_buffers[0] so attribute 0
    // has an enabled array — WebGL2 otherwise warns about expensive emulation.
    static const float fullscreen_tri[6] = {
        -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f,
    };
    sg_buffer_desc bdesc{};
    bdesc.usage.vertex_buffer = true;
    bdesc.data = SG_RANGE(fullscreen_tri);
    bdesc.label = "ao_fullscreen_tri";
    vbuf_ = sg_make_buffer(&bdesc);

    // 1×1 white R8 dummy. Bound by the composite when AO is disabled so the
    // pipeline keeps a single binding contract regardless of toggle state.
    static const uint8_t white = 0xff;
    sg_image_desc idesc{};
    idesc.type = SG_IMAGETYPE_2D;
    idesc.width = 1;
    idesc.height = 1;
    idesc.num_mipmaps = 1;
    idesc.pixel_format = SG_PIXELFORMAT_R8;
    idesc.data.mip_levels[0].ptr = &white;
    idesc.data.mip_levels[0].size = sizeof(white);
    idesc.label = "ao_dummy_white";
    dummy_image_ = sg_make_image(&idesc);

    sg_view_desc vdesc{};
    vdesc.texture.image = dummy_image_;
    vdesc.label = "ao_dummy_view";
    dummy_view_ = sg_make_view(&vdesc);

    sg_sampler_desc sdesc{};
    sdesc.min_filter = SG_FILTER_NEAREST;
    sdesc.mag_filter = SG_FILTER_NEAREST;
    sdesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.label = "ao_dummy_sampler";
    dummy_sampler_ = sg_make_sampler(&sdesc);

    initialized_ = true;
}

void AoPass::setTargetColorFormat(sg_pixel_format fmt) {
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
    pdesc.layout.attrs[ATTR_ao_ao_a_pos].buffer_index = 0;
    pdesc.layout.attrs[ATTR_ao_ao_a_pos].format = SG_VERTEXFORMAT_FLOAT2;
    pdesc.layout.attrs[ATTR_ao_ao_a_pos].offset = 0;
    pdesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pdesc.cull_mode = SG_CULLMODE_NONE;
    pdesc.depth.write_enabled = false;
    pdesc.depth.compare = SG_COMPAREFUNC_ALWAYS;
    // The AO pass renders into a color-only attachment (no depth/stencil).
    // Sokol's pipeline default is DEPTH_STENCIL, which WebGPU rejects as
    // incompatible with the depthless render pass. Pin to NONE explicitly.
    pdesc.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pdesc.color_count = 1;
    pdesc.colors[0].pixel_format = fmt;
    pdesc.label = "ao_pipeline";
    pipeline_ = sg_make_pipeline(&pdesc);
    current_color_format_ = fmt;
}

void AoPass::release() {
    if (!initialized_) {
        return;
    }
    if (dummy_sampler_.id != SG_INVALID_ID) {
        sg_destroy_sampler(dummy_sampler_);
        dummy_sampler_ = sg_sampler{};
    }
    if (dummy_view_.id != SG_INVALID_ID) {
        sg_destroy_view(dummy_view_);
        dummy_view_ = sg_view{};
    }
    if (dummy_image_.id != SG_INVALID_ID) {
        sg_destroy_image(dummy_image_);
        dummy_image_ = sg_image{};
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

void AoPass::draw(const SceneRenderTarget &scene_rt, const Camera &camera, uint32_t target_w,
                  uint32_t target_h, const RenderQualitySettings &quality) {
    if (!initialized_ || pipeline_.id == SG_INVALID_ID || target_w == 0 || target_h == 0 ||
        scene_rt.depth.id == SG_INVALID_ID) {
        return;
    }

    sg_apply_pipeline(pipeline_);

    sg_bindings bind{};
    bind.vertex_buffers[0] = vbuf_;
    bind.views[VIEW_ao_scene_depth] = scene_rt.depth_texture_view;
    bind.samplers[SMP_ao_smp_depth] = scene_rt.depth_sampler;
    sg_apply_bindings(&bind);

    // Compute view-ray reconstruction params from camera. For perspective
    // we ship `tan(fov/2) * aspect, tan(fov/2)` and the shader scales by
    // depth. For orthographic the per-pixel ray is constant: ship the
    // half-width / half-height matching glm::ortho's bounds, and the shader
    // skips the depth multiply.
    const float fov_rad = camera.fov_deg * 0.017453292519943295f; // π/180
    const float tan_half = std::tan(fov_rad * 0.5f);
    const float aspect = static_cast<float>(target_w) / static_cast<float>(target_h);
    const bool is_perspective = camera.projection == ProjectionMode::Perspective;

    ao_ao_params_block_t params{};
    if (is_perspective) {
        params.inv_proj[0] = tan_half * aspect;
        params.inv_proj[1] = tan_half;
    } else {
        // Match camera.cpp::proj()'s ortho box derivation:
        //   half_height = distance * tan(fov/2),  half_width = half_h * aspect.
        const float half_height = camera.distance * tan_half;
        const float half_width = half_height * aspect;
        params.inv_proj[0] = half_width;
        params.inv_proj[1] = half_height;
    }
    params.inv_proj[2] = 0.0f;
    params.inv_proj[3] = 0.0f;

    // Same convention as composite's depth_params: 0=normalZ, 1=reversedZ,
    // 2=logZ. Mutually exclusive; useLogDepth() takes priority on GLES3.
    const float depth_mode = useLogDepth() ? 2.0f : (useReversedZ() ? 1.0f : 0.0f);
    params.depth_params[0] = depth_mode;
    params.depth_params[1] = camera.far_plane;
    params.depth_params[2] = camera.near_plane;
    params.depth_params[3] = 0.0f;

    const float inv_w = 1.0f / static_cast<float>(target_w);
    const float inv_h = 1.0f / static_cast<float>(target_h);
    params.ao_params[0] = quality.ao_intensity;
    params.ao_params[1] = quality.ao_radius;
    params.ao_params[2] = inv_w;
    params.ao_params[3] = inv_h;

    // Map the preset enum onto (slices, steps) pairs. Keep this table in
    // sync with the preset enum doc-comment in render_quality.hpp. The
    // shader caps both values at compile-time constants (NUM_SLICES_MAX,
    // NUM_STEPS_MAX = 8), so anything beyond Ultra would be silently
    // clamped — we don't expose options past that for a reason.
    int slices = 4;
    int steps = 4;
    switch (quality.ao_quality) {
    case AoQualityPreset::Low:
        slices = 4;
        steps = 3;
        break;
    case AoQualityPreset::Medium:
        slices = 4;
        steps = 4;
        break;
    case AoQualityPreset::High:
        slices = 6;
        steps = 6;
        break;
    case AoQualityPreset::Ultra:
        slices = 8;
        steps = 8;
        break;
    }
    params.ao_quality[0] = static_cast<float>(slices);
    params.ao_quality[1] = static_cast<float>(steps);
    params.ao_quality[2] = 0.0f;
    params.ao_quality[3] = 0.0f;

    // Match composite's flip_v so the AO map and scene_color stay aligned
    // when the composite samples them with a single uv. is_perspective and
    // thickness drive the horizon-rejection logic in the FS.
    params.frame_params[0] = sg_query_features().origin_top_left ? 1.0f : 0.0f;
    params.frame_params[1] = is_perspective ? 1.0f : 0.0f;
    params.frame_params[2] = quality.ao_thickness;
    params.frame_params[3] = 0.0f;

    // World-from-view matrix. The FS accumulates the bent normal in view
    // space (the natural frame of the GTAO integration) and converts to
    // world before octahedral-encoding, so downstream consumers can sample
    // it without a view matrix in hand. glm::affineInverse is safe here
    // because the view matrix is a rigid transform.
    const glm::mat4 view = camera.view();
    const glm::mat4 inv_view = glm::affineInverse(view);
    std::memcpy(params.inv_view, glm::value_ptr(inv_view), sizeof(params.inv_view));

    sg_range u{&params, sizeof(params)};
    sg_apply_uniforms(UB_ao_ao_params_block, &u);

    sg_draw(0, 3, 1);
}

} // namespace nodehammer::viewer
