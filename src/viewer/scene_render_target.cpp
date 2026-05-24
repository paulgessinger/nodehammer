#include "scene_render_target.hpp"

namespace nodehammer::viewer {

void SceneRenderTarget::create(uint32_t w, uint32_t h, sg_pixel_format color_fmt,
                               sg_pixel_format depth_fmt) {
    destroy();

    width = w;
    height = h;
    color_format = color_fmt;
    depth_format = depth_fmt;

    sg_image_desc cdesc{};
    cdesc.usage.color_attachment = true;
    cdesc.width = static_cast<int>(w);
    cdesc.height = static_cast<int>(h);
    cdesc.pixel_format = color_fmt;
    cdesc.sample_count = 1;
    cdesc.label = "scene_color";
    color = sg_make_image(&cdesc);

    sg_image_desc ddesc{};
    ddesc.usage.depth_stencil_attachment = true;
    ddesc.width = static_cast<int>(w);
    ddesc.height = static_cast<int>(h);
    ddesc.pixel_format = depth_fmt;
    ddesc.sample_count = 1;
    ddesc.label = "scene_depth";
    depth = sg_make_image(&ddesc);

    // Attachment views — bound by the scene pass.
    sg_view_desc cv_att_desc{};
    cv_att_desc.color_attachment.image = color;
    cv_att_desc.label = "scene_color_attachment_view";
    color_attachment_view = sg_make_view(&cv_att_desc);

    sg_view_desc dv_att_desc{};
    dv_att_desc.depth_stencil_attachment.image = depth;
    dv_att_desc.label = "scene_depth_attachment_view";
    depth_attachment_view = sg_make_view(&dv_att_desc);

    // Texture views — sampled by the composite pass. Sokol allows
    // creating a texture view from an attachment image implicitly
    // (no extra usage flag required).
    sg_view_desc cv_tex_desc{};
    cv_tex_desc.texture.image = color;
    cv_tex_desc.label = "scene_color_texture_view";
    color_texture_view = sg_make_view(&cv_tex_desc);

    sg_view_desc dv_tex_desc{};
    dv_tex_desc.texture.image = depth;
    dv_tex_desc.label = "scene_depth_texture_view";
    depth_texture_view = sg_make_view(&dv_tex_desc);

    sg_sampler_desc sdesc{};
    sdesc.min_filter = SG_FILTER_LINEAR;
    sdesc.mag_filter = SG_FILTER_LINEAR;
    sdesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.label = "scene_target_sampler";
    sampler = sg_make_sampler(&sdesc);

    // WebGPU forbids filtering samplers on Depth32Float textures (and the
    // sokol validation layer enforces the same rule across backends). Use
    // a nonfiltering sampler dedicated to depth sampling in the composite.
    sg_sampler_desc dsdesc{};
    dsdesc.min_filter = SG_FILTER_NEAREST;
    dsdesc.mag_filter = SG_FILTER_NEAREST;
    dsdesc.mipmap_filter = SG_FILTER_NEAREST;
    dsdesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    dsdesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    dsdesc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    dsdesc.label = "scene_target_depth_sampler";
    depth_sampler = sg_make_sampler(&dsdesc);
}

void SceneRenderTarget::destroy() {
    if (sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(sampler);
        sampler = sg_sampler{};
    }
    if (depth_sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(depth_sampler);
        depth_sampler = sg_sampler{};
    }
    if (color_attachment_view.id != SG_INVALID_ID) {
        sg_destroy_view(color_attachment_view);
        color_attachment_view = sg_view{};
    }
    if (depth_attachment_view.id != SG_INVALID_ID) {
        sg_destroy_view(depth_attachment_view);
        depth_attachment_view = sg_view{};
    }
    if (color_texture_view.id != SG_INVALID_ID) {
        sg_destroy_view(color_texture_view);
        color_texture_view = sg_view{};
    }
    if (depth_texture_view.id != SG_INVALID_ID) {
        sg_destroy_view(depth_texture_view);
        depth_texture_view = sg_view{};
    }
    if (color.id != SG_INVALID_ID) {
        sg_destroy_image(color);
        color = sg_image{};
    }
    if (depth.id != SG_INVALID_ID) {
        sg_destroy_image(depth);
        depth = sg_image{};
    }
    width = 0;
    height = 0;
}

bool SceneRenderTarget::matches(uint32_t w, uint32_t h, sg_pixel_format color_fmt,
                                sg_pixel_format depth_fmt) const {
    return color.id != SG_INVALID_ID && width == w && height == h && color_format == color_fmt &&
           depth_format == depth_fmt;
}

sg_attachments SceneRenderTarget::passAttachments() const {
    sg_attachments a{};
    a.colors[0] = color_attachment_view;
    a.depth_stencil = depth_attachment_view;
    return a;
}

} // namespace nodehammer::viewer
