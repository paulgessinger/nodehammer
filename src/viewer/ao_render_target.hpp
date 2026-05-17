#pragma once

#include <sokol_gfx.h>

#include <cstdint>

namespace nodehammer::viewer {

/// RGBA8 offscreen target for the GTAO pass. Mirrors the `SceneRenderTarget`
/// shape (attachment view + texture view + linear sampler) but holds no depth
/// — AO disables depth test/write entirely.
///
/// Channel layout (after GTAO):
///   R    — ambient occlusion (1.0 = unoccluded, 0.0 = fully occluded)
///   G, B — octahedral-encoded *bent normal* in world space (the
///          mean-unoccluded direction; falls back to N when fully open)
///   A    — currently unused (reserved for confidence / cone aperture)
///
/// Two instances are allocated by the App: one "raw" target written by the
/// GTAO pass, and one "history" target written by the denoise pass. The
/// history target is also read by the *next* frame's scene shader (PBR IBL
/// path) and by the current frame's composite (Lambert fallback path).
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
    sg_pixel_format color_format{SG_PIXELFORMAT_RGBA8};

    /// Allocate (or reallocate, after destroy()) the AO image at the given
    /// size and format. RGBA8 is the only supported format today — see the
    /// channel-layout comment above; the format parameter exists so the
    /// caller can still gate on `pickAoColorFormat()` for "what format will
    /// I actually get" without two reallocation paths.
    void create(uint32_t w, uint32_t h, sg_pixel_format fmt);

    /// Release every sokol handle held. Idempotent.
    void destroy();

    /// True when the existing resources match the requested size and format.
    [[nodiscard]] bool matches(uint32_t w, uint32_t h, sg_pixel_format fmt) const;

    /// Build the `sg_attachments` value the AO/denoise pass should use
    /// (color only).
    [[nodiscard]] sg_attachments passAttachments() const;
};

/// Pick the AO color format. Always RGBA8 today — GTAO now packs both
/// occlusion (R) and an octahedral-encoded bent normal (G,B) into the same
/// target, and the bent-normal channels need at least 8 bits each per axis.
/// A future RGBA16F variant would be needed to revisit the R-channel
/// precision win that R16F had pre-bent-normal.
[[nodiscard]] sg_pixel_format pickAoColorFormat();

} // namespace nodehammer::viewer
