#pragma once

#include <sokol_gfx.h>

#include <cstdint>

namespace nodehammer::viewer {

/// Single-channel R8 offscreen target for the GTAO pass. Mirrors the
/// `SceneRenderTarget` shape (attachment view + texture view + linear sampler)
/// but holds no depth — AO disables depth test/write entirely.
///
/// Allocated lazily by `App::Impl::ensureAoTarget` only when `enable_ao` is on,
/// and reallocated on framebuffer-size change.
struct AoRenderTarget {
    sg_image color{};
    sg_view color_attachment_view{};
    sg_view color_texture_view{};
    sg_sampler sampler{}; // LINEAR — composite samples the AO map bilinearly
    uint32_t width{0};
    uint32_t height{0};
    sg_pixel_format color_format{SG_PIXELFORMAT_R8};

    /// Allocate (or reallocate, after destroy()) the AO image at the given
    /// size and format. R16F is preferred when the backend can render+filter
    /// it (10-bit-equivalent precision kills the gradient banding R8 shows
    /// on smooth surfaces); R8 is the fallback path. The caller picks the
    /// format via `pickAoColorFormat()` so the WebGL2 / older-driver gate
    /// stays in one place.
    void create(uint32_t w, uint32_t h, sg_pixel_format fmt);

    /// Release every sokol handle held. Idempotent.
    void destroy();

    /// True when the existing resources match the requested size and format.
    [[nodiscard]] bool matches(uint32_t w, uint32_t h, sg_pixel_format fmt) const;

    /// Build the `sg_attachments` value the AO pass should use (color only).
    [[nodiscard]] sg_attachments passAttachments() const;
};

/// Pick the best AO color format the backend supports: R16F when it is
/// renderable and filterable, R8 otherwise. Filterable matters because
/// composite samples the AO map bilinearly — and a future half-resolution
/// AO + bilateral upsample step depends on it.
[[nodiscard]] sg_pixel_format pickAoColorFormat();

} // namespace nodehammer::viewer
