#include "ao_render_target.hpp"

namespace nodehammer::viewer {

void AoRenderTarget::create(uint32_t w, uint32_t h, sg_pixel_format fmt) {
    destroy();

    width = w;
    height = h;
    color_format = fmt;

    sg_image_desc cdesc{};
    cdesc.usage.color_attachment = true;
    cdesc.width = static_cast<int>(w);
    cdesc.height = static_cast<int>(h);
    cdesc.pixel_format = fmt;
    cdesc.sample_count = 1;
    cdesc.label = "ao_color";
    color = sg_make_image(&cdesc);

    sg_view_desc att_desc{};
    att_desc.color_attachment.image = color;
    att_desc.label = "ao_color_attachment_view";
    color_attachment_view = sg_make_view(&att_desc);

    sg_view_desc tex_desc{};
    tex_desc.texture.image = color;
    tex_desc.label = "ao_color_texture_view";
    color_texture_view = sg_make_view(&tex_desc);

    sg_sampler_desc sdesc{};
    sdesc.min_filter = SG_FILTER_LINEAR;
    sdesc.mag_filter = SG_FILTER_LINEAR;
    sdesc.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.label = "ao_sampler";
    sampler = sg_make_sampler(&sdesc);
}

void AoRenderTarget::destroy() {
    if (sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(sampler);
        sampler = sg_sampler{};
    }
    if (color_attachment_view.id != SG_INVALID_ID) {
        sg_destroy_view(color_attachment_view);
        color_attachment_view = sg_view{};
    }
    if (color_texture_view.id != SG_INVALID_ID) {
        sg_destroy_view(color_texture_view);
        color_texture_view = sg_view{};
    }
    if (color.id != SG_INVALID_ID) {
        sg_destroy_image(color);
        color = sg_image{};
    }
    width = 0;
    height = 0;
}

bool AoRenderTarget::matches(uint32_t w, uint32_t h, sg_pixel_format fmt) const {
    return color.id != SG_INVALID_ID && width == w && height == h && color_format == fmt;
}

sg_attachments AoRenderTarget::passAttachments() const {
    sg_attachments a{};
    a.colors[0] = color_attachment_view;
    return a;
}

sg_pixel_format pickAoColorFormat() {
    // Prefer R16F when both renderable and filterable (~10-bit precision in
    // [0,1] vs R8's 256 levels — eliminates the gradient banding visible on
    // smooth surfaces under R8). Fall back to R8 on backends that can't
    // render or filter half-floats (typically WebGL2 without
    // EXT_color_buffer_half_float / OES_texture_half_float_linear).
    const sg_pixelformat_info f16 = sg_query_pixelformat(SG_PIXELFORMAT_R16F);
    if (f16.render && f16.filter) {
        return SG_PIXELFORMAT_R16F;
    }
    return SG_PIXELFORMAT_R8;
}

} // namespace nodehammer::viewer
