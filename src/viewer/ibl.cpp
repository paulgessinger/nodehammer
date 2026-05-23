#include "ibl.hpp"

#include <nodehammer/viewer/backend_caps.hpp>

#include <array>
#include <cstring>

#include "ibl_bake.glsl.h"

namespace nodehammer::viewer {

namespace {

constexpr int kBrdfLutSize = IblBakeData::kBrdfLutSize;
constexpr int kIrradianceSize = IblBakeData::kIrradianceSize;
constexpr int kPrefilterSize = IblBakeData::kPrefilterSize;
constexpr int kPrefilterMips = IblBakeData::kPrefilterMips;

// Pick the bake format for the irradiance + prefilter cubemaps. Order of
// preference: RGBA32F → RGBA16F → RGBA8.
//
// RGBA32F is preferred so the GGX importance sampling integration runs
// against a *full-fp32* hardware filter — fp16 filtering on RGBA16F has
// per-tap rounding error that biases differently across Chrome's
// WebGL2/ANGLE→Metal and WebGPU→Metal driver paths, producing a visible
// color cast between backends after 1000+ samples per output texel.
// RGBA32F filtering keeps the bake bit-equivalent across backends.
//
// RGBA32F filterability requires:
//   - WebGL2: OES_texture_float_linear extension
//   - WebGPU: the `float32-filterable` feature
// Where unavailable, fall back to RGBA16F (still HDR-capable, just with
// the per-backend variance). RGBA8 is the last-resort LDR fallback.
//
// The BRDF LUT stays RGBA8 — it's a [0,1]-bounded function of roughness
// and n·v with no cubemap filtering involved.
sg_pixel_format pickIblBakeFormat() {
    // The bake does single-write fullscreen passes per face/mip with the
    // default-off pipeline blend state, so `info.blend` is intentionally
    // not part of the gate — it would falsely disqualify RGBA32F on WebGPU
    // where the `float32-blendable` feature is rarely enabled even when
    // `float32-filterable` (the cap we actually need) is present.
    sg_pixelformat_info info32 = sg_query_pixelformat(SG_PIXELFORMAT_RGBA32F);
    if (info32.render && info32.filter) {
        return SG_PIXELFORMAT_RGBA32F;
    }
    sg_pixelformat_info info16 = sg_query_pixelformat(SG_PIXELFORMAT_RGBA16F);
    if (info16.render && info16.filter) {
        return SG_PIXELFORMAT_RGBA16F;
    }
    return SG_PIXELFORMAT_RGBA8;
}

sg_image makeColorAttachment(sg_image_type type, int size, int num_mipmaps, sg_pixel_format fmt,
                             const char *label) {
    sg_image_desc desc{};
    desc.type = type;
    desc.usage.color_attachment = true;
    desc.width = size;
    desc.height = size;
    desc.num_mipmaps = num_mipmaps;
    desc.pixel_format = fmt;
    desc.sample_count = 1;
    desc.label = label;
    return sg_make_image(&desc);
}

sg_view makeColorAttachmentView(sg_image image, int mip_level, int slice) {
    sg_view_desc desc{};
    desc.color_attachment.image = image;
    desc.color_attachment.mip_level = mip_level;
    desc.color_attachment.slice = slice;
    return sg_make_view(&desc);
}

sg_pipeline makeBakePipeline(sg_shader shader, int attr_pos_slot, sg_pixel_format fmt,
                             const char *label) {
    sg_pipeline_desc pdesc{};
    pdesc.shader = shader;
    pdesc.layout.buffers[0].stride = static_cast<int>(sizeof(float) * 2);
    pdesc.layout.attrs[attr_pos_slot].buffer_index = 0;
    pdesc.layout.attrs[attr_pos_slot].format = SG_VERTEXFORMAT_FLOAT2;
    pdesc.layout.attrs[attr_pos_slot].offset = 0;
    pdesc.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pdesc.cull_mode = SG_CULLMODE_NONE;
    pdesc.depth.pixel_format = SG_PIXELFORMAT_NONE;
    pdesc.depth.write_enabled = false;
    pdesc.color_count = 1;
    pdesc.colors[0].pixel_format = fmt;
    pdesc.label = label;
    return sg_make_pipeline(&pdesc);
}

void runPass(sg_view color_view, sg_pipeline pipe, sg_buffer vbuf, const sg_range *uniforms,
             int ub_slot) {
    sg_pass pass{};
    pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
    pass.action.colors[0].store_action = SG_STOREACTION_STORE;
    pass.action.depth.load_action = SG_LOADACTION_DONTCARE;
    pass.action.depth.store_action = SG_STOREACTION_DONTCARE;
    pass.attachments.colors[0] = color_view;
    sg_begin_pass(&pass);
    sg_apply_pipeline(pipe);
    sg_bindings bind{};
    bind.vertex_buffers[0] = vbuf;
    sg_apply_bindings(&bind);
    if (uniforms != nullptr) {
        sg_apply_uniforms(ub_slot, uniforms);
    }
    sg_draw(0, 3, 1);
    sg_end_pass();
}

ibl_bake_ibl_settings_t makeUniforms(const IblSettings &s, int face, float roughness, int samples) {
    ibl_bake_ibl_settings_t u{};
    u.face_rough_samples[0] = static_cast<float>(face);
    u.face_rough_samples[1] = roughness;
    u.face_rough_samples[2] = static_cast<float>(samples > 0 ? samples : 1);
    // Cubemap face V-flip. The cube sampler's t-axis runs top→bottom of the
    // face per RenderMan convention (Vulkan/D3D/Metal +Y face: t=0 ↔ rz=-1,
    // t=1 ↔ rz=+1). On origin_top_left backends (Metal/D3D/WGPU) NDC v=+1
    // writes to texture row t=0, so cubeDir(2, u, +1) — which produces
    // rz=+1 — ends up at the row the sampler reads for rz=-1, giving the
    // upside-down sky symptom on wall faces. The bake FS negates v before
    // cubeDir to compensate. On origin_top_left=false (GL family) NDC v=+1
    // writes to memory row y=max which the cube sampler addresses as t=1,
    // matching cubeDir's rz=+1 directly — no flip needed.
    u.face_rough_samples[3] = sg_query_features().origin_top_left ? 1.0f : 0.0f;
    u.zenith_color[0] = s.zenith_color.r;
    u.zenith_color[1] = s.zenith_color.g;
    u.zenith_color[2] = s.zenith_color.b;
    u.horizon_color[0] = s.horizon_color.r;
    u.horizon_color[1] = s.horizon_color.g;
    u.horizon_color[2] = s.horizon_color.b;
    u.ground_color[0] = s.ground_color.r;
    u.ground_color[1] = s.ground_color.g;
    u.ground_color[2] = s.ground_color.b;
    // sun_dir.w carries sun_intensity; both sky models multiply by it as a
    // global radiance scalar. RGBA16F bake targets let the disc exceed 1.0,
    // which is what HDR + tonemap responds to.
    u.sun_dir[0] = s.sun_dir.x;
    u.sun_dir[1] = s.sun_dir.y;
    u.sun_dir[2] = s.sun_dir.z;
    u.sun_dir[3] = s.sun_intensity;
    u.sun_color[0] = s.sun_color.r;
    u.sun_color[1] = s.sun_color.g;
    u.sun_color[2] = s.sun_color.b;
    u.sun_color[3] = s.sun_sharpness;
    u.ground_albedo[0] = s.ground_albedo.r;
    u.ground_albedo[1] = s.ground_albedo.g;
    u.ground_albedo[2] = s.ground_albedo.b;
    u.sky_params[0] = s.turbidity;
    u.sky_params[1] = static_cast<float>(s.sky_model);
    return u;
}

} // namespace

IblBakeData bakeIblGpu(const IblSettings &settings) {
    IblBakeData out;
    const sg_pixel_format env_fmt = pickIblBakeFormat();
    out.brdf_lut = SharedImage{makeColorAttachment(SG_IMAGETYPE_2D, kBrdfLutSize, 1,
                                                   SG_PIXELFORMAT_RGBA8, "ibl_brdf_lut")};
    out.irradiance = SharedImage{
        makeColorAttachment(SG_IMAGETYPE_CUBE, kIrradianceSize, 1, env_fmt, "ibl_irradiance")};
    out.prefilter = SharedImage{makeColorAttachment(SG_IMAGETYPE_CUBE, kPrefilterSize,
                                                    kPrefilterMips, env_fmt, "ibl_prefilter")};
    out.prefilter_mip_count = kPrefilterMips;

    // Fullscreen triangle covering clip-space [-1, 1]^2 — the FS reconstructs
    // (u, v) ∈ [-1, 1] from gl_FragCoord/v_uv to drive cube-direction mapping.
    const std::array<float, 6> tri_verts = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
    sg_buffer_desc vdesc{};
    vdesc.usage.vertex_buffer = true;
    vdesc.usage.immutable = true;
    vdesc.data.ptr = tri_verts.data();
    vdesc.data.size = tri_verts.size() * sizeof(float);
    vdesc.label = "ibl_bake_fst_vbuf";
    sg_buffer vbuf = sg_make_buffer(&vdesc);

    const sg_backend backend = sg_query_backend();
    sg_shader sh_brdf = sg_make_shader(ibl_bake_ibl_brdf_shader_desc(backend));
    sg_shader sh_irr = sg_make_shader(ibl_bake_ibl_irr_shader_desc(backend));
    sg_shader sh_pre = sg_make_shader(ibl_bake_ibl_pre_shader_desc(backend));

    sg_pipeline pipe_brdf = makeBakePipeline(sh_brdf, ATTR_ibl_bake_ibl_brdf_a_pos,
                                             SG_PIXELFORMAT_RGBA8, "ibl_brdf_pipe");
    sg_pipeline pipe_irr =
        makeBakePipeline(sh_irr, ATTR_ibl_bake_ibl_irr_a_pos, env_fmt, "ibl_irr_pipe");
    sg_pipeline pipe_pre =
        makeBakePipeline(sh_pre, ATTR_ibl_bake_ibl_pre_a_pos, env_fmt, "ibl_pre_pipe");

    // BRDF LUT — single pass. (Sky params are unused but the shared block is bound anyway.)
    {
        const auto u = makeUniforms(settings, 0, 0.f, settings.brdf_samples);
        const sg_range range{&u, sizeof(u)};
        sg_view view = makeColorAttachmentView(out.brdf_lut.get(), 0, 0);
        runPass(view, pipe_brdf, vbuf, &range, UB_ibl_bake_ibl_settings);
        sg_destroy_view(view);
    }

    // Irradiance cubemap — one pass per face.
    for (int face = 0; face < 6; ++face) {
        const auto u = makeUniforms(settings, face, 0.f, settings.irradiance_samples);
        const sg_range range{&u, sizeof(u)};
        sg_view view = makeColorAttachmentView(out.irradiance.get(), 0, face);
        runPass(view, pipe_irr, vbuf, &range, UB_ibl_bake_ibl_settings);
        sg_destroy_view(view);
    }

    // Prefiltered specular cubemap — one pass per (face, mip).
    for (int mip = 0; mip < kPrefilterMips; ++mip) {
        const float roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
        for (int face = 0; face < 6; ++face) {
            const auto u = makeUniforms(settings, face, roughness, settings.prefilter_samples);
            const sg_range range{&u, sizeof(u)};
            sg_view view = makeColorAttachmentView(out.prefilter.get(), mip, face);
            runPass(view, pipe_pre, vbuf, &range, UB_ibl_bake_ibl_settings);
            sg_destroy_view(view);
        }
    }

    sg_destroy_pipeline(pipe_brdf);
    sg_destroy_pipeline(pipe_irr);
    sg_destroy_pipeline(pipe_pre);
    sg_destroy_shader(sh_brdf);
    sg_destroy_shader(sh_irr);
    sg_destroy_shader(sh_pre);
    sg_destroy_buffer(vbuf);

    return out;
}

void IblResources::createDummy() {
    // 1x1 RGBA8 placeholders so sg_bindings always have something valid to
    // sample from. The scene shader gates IBL contributions on mode_flags.x,
    // so these values are never visible in normal use; they exist so the
    // first frame can render before bakeIblGpu() runs.

    const std::array<std::byte, 4> lut_pixel{std::byte{255}, std::byte{0}, std::byte{0},
                                             std::byte{255}};
    sg_image_desc lut_desc{};
    lut_desc.type = SG_IMAGETYPE_2D;
    lut_desc.width = 1;
    lut_desc.height = 1;
    lut_desc.num_mipmaps = 1;
    lut_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    lut_desc.data.mip_levels[0].ptr = lut_pixel.data();
    lut_desc.data.mip_levels[0].size = lut_pixel.size();
    lut_desc.label = "ibl_brdf_lut_dummy";
    brdf_lut = SharedImage{sg_make_image(&lut_desc)};

    std::array<std::byte, 4 * 6> irr_pixels{};
    for (size_t f = 0; f < 6; ++f) {
        irr_pixels[f * 4 + 0] = std::byte{128};
        irr_pixels[f * 4 + 1] = std::byte{128};
        irr_pixels[f * 4 + 2] = std::byte{128};
        irr_pixels[f * 4 + 3] = std::byte{255};
    }
    sg_image_desc irr_desc{};
    irr_desc.type = SG_IMAGETYPE_CUBE;
    irr_desc.width = 1;
    irr_desc.height = 1;
    irr_desc.num_mipmaps = 1;
    irr_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    irr_desc.data.mip_levels[0].ptr = irr_pixels.data();
    irr_desc.data.mip_levels[0].size = irr_pixels.size();
    irr_desc.label = "ibl_irradiance_dummy";
    irradiance = SharedImage{sg_make_image(&irr_desc)};

    sg_image_desc pre_desc{};
    pre_desc.type = SG_IMAGETYPE_CUBE;
    pre_desc.width = 1;
    pre_desc.height = 1;
    pre_desc.num_mipmaps = 1;
    pre_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    pre_desc.data.mip_levels[0].ptr = irr_pixels.data();
    pre_desc.data.mip_levels[0].size = irr_pixels.size();
    pre_desc.label = "ibl_prefilter_dummy";
    prefilter = SharedImage{sg_make_image(&pre_desc)};
    prefilter_mip_count = 1;

    sg_view_desc irr_view{};
    irr_view.texture.image = irradiance.get();
    irr_view.label = "ibl_irradiance_view";
    irradiance_view = sg_make_view(&irr_view);

    sg_view_desc pre_view{};
    pre_view.texture.image = prefilter.get();
    pre_view.label = "ibl_prefilter_view";
    prefilter_view = sg_make_view(&pre_view);

    sg_view_desc lut_view{};
    lut_view.texture.image = brdf_lut.get();
    lut_view.label = "ibl_brdf_lut_view";
    brdf_lut_view = sg_make_view(&lut_view);

    sg_sampler_desc cube_smp{};
    cube_smp.min_filter = SG_FILTER_LINEAR;
    cube_smp.mag_filter = SG_FILTER_LINEAR;
    cube_smp.mipmap_filter = SG_FILTER_LINEAR;
    cube_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.label = "ibl_cube_sampler";
    cube_sampler = sg_make_sampler(&cube_smp);

    sg_sampler_desc lut_smp{};
    lut_smp.min_filter = SG_FILTER_LINEAR;
    lut_smp.mag_filter = SG_FILTER_LINEAR;
    lut_smp.mipmap_filter = SG_FILTER_NEAREST;
    lut_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.label = "ibl_lut_sampler";
    lut_sampler = sg_make_sampler(&lut_smp);
}

void IblResources::upload(const IblBakeData &data) {
    // Share ownership of the baked images (refcount bump) — this same bake may
    // also be installed in another renderer's IblResources.
    brdf_lut = data.brdf_lut;
    irradiance = data.irradiance;
    prefilter = data.prefilter;
    prefilter_mip_count = data.prefilter_mip_count;

    sg_view_desc irr_view{};
    irr_view.texture.image = irradiance.get();
    irr_view.label = "ibl_irradiance_view";
    irradiance_view = sg_make_view(&irr_view);

    sg_view_desc pre_view{};
    pre_view.texture.image = prefilter.get();
    pre_view.label = "ibl_prefilter_view";
    prefilter_view = sg_make_view(&pre_view);

    sg_view_desc lut_view{};
    lut_view.texture.image = brdf_lut.get();
    lut_view.label = "ibl_brdf_lut_view";
    brdf_lut_view = sg_make_view(&lut_view);

    sg_sampler_desc cube_smp{};
    cube_smp.min_filter = SG_FILTER_LINEAR;
    cube_smp.mag_filter = SG_FILTER_LINEAR;
    cube_smp.mipmap_filter = SG_FILTER_LINEAR;
    cube_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.label = "ibl_cube_sampler";
    cube_sampler = sg_make_sampler(&cube_smp);

    sg_sampler_desc lut_smp{};
    lut_smp.min_filter = SG_FILTER_LINEAR;
    lut_smp.mag_filter = SG_FILTER_LINEAR;
    lut_smp.mipmap_filter = SG_FILTER_NEAREST;
    lut_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.label = "ibl_lut_sampler";
    lut_sampler = sg_make_sampler(&lut_smp);
}

void IblResources::release() {
    if (irradiance_view.id != SG_INVALID_ID) {
        sg_destroy_view(irradiance_view);
        irradiance_view = sg_view{};
    }
    if (prefilter_view.id != SG_INVALID_ID) {
        sg_destroy_view(prefilter_view);
        prefilter_view = sg_view{};
    }
    if (brdf_lut_view.id != SG_INVALID_ID) {
        sg_destroy_view(brdf_lut_view);
        brdf_lut_view = sg_view{};
    }
    // Drop our references to the images; sg_destroy_image fires only when the
    // last SharedImage (across all renderers sharing this bake) is released.
    // Views above are destroyed first so no view outlives its image.
    irradiance.reset();
    prefilter.reset();
    brdf_lut.reset();
    if (cube_sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(cube_sampler);
        cube_sampler = sg_sampler{};
    }
    if (lut_sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(lut_sampler);
        lut_sampler = sg_sampler{};
    }
    prefilter_mip_count = 0;
}

} // namespace nodehammer::viewer
