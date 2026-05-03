#include "ibl.hpp"

#include <array>
#include <cstring>

#include "ibl_bake.glsl.h"

namespace nodehammer::viewer {

namespace {

constexpr int kBrdfLutSize = IblBakeData::kBrdfLutSize;
constexpr int kIrradianceSize = IblBakeData::kIrradianceSize;
constexpr int kPrefilterSize = IblBakeData::kPrefilterSize;
constexpr int kPrefilterMips = IblBakeData::kPrefilterMips;

sg_image makeColorAttachment(sg_image_type type, int size, int num_mipmaps, const char *label) {
    sg_image_desc desc{};
    desc.type = type;
    desc.usage.color_attachment = true;
    desc.width = size;
    desc.height = size;
    desc.num_mipmaps = num_mipmaps;
    desc.pixel_format = SG_PIXELFORMAT_RGBA8;
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

sg_pipeline makeBakePipeline(sg_shader shader, int attr_pos_slot, const char *label) {
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
    pdesc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
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
    u.zenith_color[0] = s.zenith_color.r;
    u.zenith_color[1] = s.zenith_color.g;
    u.zenith_color[2] = s.zenith_color.b;
    u.horizon_color[0] = s.horizon_color.r;
    u.horizon_color[1] = s.horizon_color.g;
    u.horizon_color[2] = s.horizon_color.b;
    u.ground_color[0] = s.ground_color.r;
    u.ground_color[1] = s.ground_color.g;
    u.ground_color[2] = s.ground_color.b;
    u.sun_dir[0] = s.sun_dir.x;
    u.sun_dir[1] = s.sun_dir.y;
    u.sun_dir[2] = s.sun_dir.z;
    u.sun_color[0] = s.sun_color.r;
    u.sun_color[1] = s.sun_color.g;
    u.sun_color[2] = s.sun_color.b;
    u.sun_color[3] = s.sun_sharpness;
    return u;
}

} // namespace

IblBakeData bakeIblGpu(const IblSettings &settings) {
    IblBakeData out;
    out.brdf_lut = makeColorAttachment(SG_IMAGETYPE_2D, kBrdfLutSize, 1, "ibl_brdf_lut");
    out.irradiance = makeColorAttachment(SG_IMAGETYPE_CUBE, kIrradianceSize, 1, "ibl_irradiance");
    out.prefilter =
        makeColorAttachment(SG_IMAGETYPE_CUBE, kPrefilterSize, kPrefilterMips, "ibl_prefilter");
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

    sg_pipeline pipe_brdf =
        makeBakePipeline(sh_brdf, ATTR_ibl_bake_ibl_brdf_a_pos, "ibl_brdf_pipe");
    sg_pipeline pipe_irr = makeBakePipeline(sh_irr, ATTR_ibl_bake_ibl_irr_a_pos, "ibl_irr_pipe");
    sg_pipeline pipe_pre = makeBakePipeline(sh_pre, ATTR_ibl_bake_ibl_pre_a_pos, "ibl_pre_pipe");

    // BRDF LUT — single pass. (Sky params are unused but the shared block is bound anyway.)
    {
        const auto u = makeUniforms(settings, 0, 0.f, settings.brdf_samples);
        const sg_range range{&u, sizeof(u)};
        sg_view view = makeColorAttachmentView(out.brdf_lut, 0, 0);
        runPass(view, pipe_brdf, vbuf, &range, UB_ibl_bake_ibl_settings);
        sg_destroy_view(view);
    }

    // Irradiance cubemap — one pass per face.
    for (int face = 0; face < 6; ++face) {
        const auto u = makeUniforms(settings, face, 0.f, settings.irradiance_samples);
        const sg_range range{&u, sizeof(u)};
        sg_view view = makeColorAttachmentView(out.irradiance, 0, face);
        runPass(view, pipe_irr, vbuf, &range, UB_ibl_bake_ibl_settings);
        sg_destroy_view(view);
    }

    // Prefiltered specular cubemap — one pass per (face, mip).
    for (int mip = 0; mip < kPrefilterMips; ++mip) {
        const float roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
        for (int face = 0; face < 6; ++face) {
            const auto u = makeUniforms(settings, face, roughness, settings.prefilter_samples);
            const sg_range range{&u, sizeof(u)};
            sg_view view = makeColorAttachmentView(out.prefilter, mip, face);
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
    brdf_lut = sg_make_image(&lut_desc);

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
    irradiance = sg_make_image(&irr_desc);

    sg_image_desc pre_desc{};
    pre_desc.type = SG_IMAGETYPE_CUBE;
    pre_desc.width = 1;
    pre_desc.height = 1;
    pre_desc.num_mipmaps = 1;
    pre_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    pre_desc.data.mip_levels[0].ptr = irr_pixels.data();
    pre_desc.data.mip_levels[0].size = irr_pixels.size();
    pre_desc.label = "ibl_prefilter_dummy";
    prefilter = sg_make_image(&pre_desc);
    prefilter_mip_count = 1;

    sg_view_desc irr_view{};
    irr_view.texture.image = irradiance;
    irr_view.label = "ibl_irradiance_view";
    irradiance_view = sg_make_view(&irr_view);

    sg_view_desc pre_view{};
    pre_view.texture.image = prefilter;
    pre_view.label = "ibl_prefilter_view";
    prefilter_view = sg_make_view(&pre_view);

    sg_view_desc lut_view{};
    lut_view.texture.image = brdf_lut;
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
    brdf_lut = data.brdf_lut;
    irradiance = data.irradiance;
    prefilter = data.prefilter;
    prefilter_mip_count = data.prefilter_mip_count;

    sg_view_desc irr_view{};
    irr_view.texture.image = irradiance;
    irr_view.label = "ibl_irradiance_view";
    irradiance_view = sg_make_view(&irr_view);

    sg_view_desc pre_view{};
    pre_view.texture.image = prefilter;
    pre_view.label = "ibl_prefilter_view";
    prefilter_view = sg_make_view(&pre_view);

    sg_view_desc lut_view{};
    lut_view.texture.image = brdf_lut;
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
    if (irradiance.id != SG_INVALID_ID) {
        sg_destroy_image(irradiance);
        irradiance = sg_image{};
    }
    if (prefilter.id != SG_INVALID_ID) {
        sg_destroy_image(prefilter);
        prefilter = sg_image{};
    }
    if (brdf_lut.id != SG_INVALID_ID) {
        sg_destroy_image(brdf_lut);
        brdf_lut = sg_image{};
    }
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
