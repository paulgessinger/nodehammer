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
    // Two AO targets coexist (raw + denoised history). Labels stay generic
    // here; the App-side caller relabels its allocations via debug groups
    // when more granularity is needed in a GPU trace.
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
    // LINEAR for AO (composite + scene shader sample bilinearly). The
    // octahedral-encoded bent normal channels also sample with LINEAR; this
    // is slightly wrong (interpolating octahedral coords doesn't strictly
    // equal interpolating the unit vector and re-encoding), but the
    // post-decode normalize() in the consumer absorbs the small error, and
    // the alternative (nearest sampling on the GB channels alone) requires
    // a second sampler binding for no visible win.
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
    // The target carries an octahedral bent normal in GB, which the scene
    // shader decodes and feeds straight into the irradiance-cubemap lookup. At
    // RGBA8 the 8-bit-per-axis octahedral encode resolves to directions ~1°
    // apart, and on a saturated-diffuse material that quantization shows up as
    // a fine color speckle on otherwise-smooth surfaces (the bent normal can't
    // be denoised away — see ao_denoise.glsl's header). RGBA16F gives ~11 bits
    // of mantissa per axis, dropping the speckle below perceptible, and also
    // smooths the AO scalar gradient (making the FS Bayer dither moot there).
    //
    // Prefer it whenever the backend can both render to and linearly filter
    // RGBA16F: WebGPU mandates it; WebGL2 exposes it via the half-float
    // color-buffer + (core) half-float linear-filter support, which sokol
    // probes at init. The renderer already uses RGBA16F for its HDR scene
    // target and IBL bakes, so this is available in practice. Fall back to
    // RGBA8 (with the AO FS's Bayer dither) on a constrained context.
    sg_pixelformat_info info = sg_query_pixelformat(SG_PIXELFORMAT_RGBA16F);
    if (info.render && info.filter) {
        return SG_PIXELFORMAT_RGBA16F;
    }
    return SG_PIXELFORMAT_RGBA8;
}

} // namespace nodehammer::viewer
