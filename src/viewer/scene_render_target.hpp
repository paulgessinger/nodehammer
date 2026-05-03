#pragma once

#include <sokol_gfx.h>

#include <cstdint>

namespace nodehammer::viewer {

/// Offscreen color + depth render target for the scene pass. The composite
/// pass samples this target into the swapchain. RAII over a small bag of
/// sokol resources; lazy creation/recreation lives in `App::Impl`.
///
/// Two views per image: an *_attachment view for the scene pass, and a
/// texture view for sampling in the composite pass. Sokol requires this
/// split — attachment views and texture views are different view types.
///
/// Reversed-Z must be preserved by the caller's pass action: the depth
/// attachment is cleared to 0.0 (the "farthest" depth) and the scene
/// pipeline uses `GREATER_EQUAL`. See scene_renderer.cpp.
struct SceneRenderTarget {
    sg_image color{};
    sg_image depth{};
    sg_view color_attachment_view{};
    sg_view depth_attachment_view{};
    sg_view color_texture_view{};
    sg_view depth_texture_view{};
    sg_sampler sampler{};       // filtering — for color
    sg_sampler depth_sampler{}; // nonfiltering — for depth (WebGPU constraint)
    uint32_t width{0};
    uint32_t height{0};
    int sample_count{1};
    sg_pixel_format color_format{SG_PIXELFORMAT_RGBA8};
    sg_pixel_format depth_format{SG_PIXELFORMAT_DEPTH};

    /// Allocate (or reallocate, after destroy()) all resources for the given
    /// size/formats/sample count. Must be called inside an active sokol_gfx
    /// session (between sg_setup / sg_shutdown).
    void create(uint32_t w, uint32_t h, sg_pixel_format color_fmt, sg_pixel_format depth_fmt,
                int samples);

    /// Release every sokol handle held. Idempotent — safe to call when the
    /// target was never created or was already destroyed.
    void destroy();

    /// True when the existing resources match the requested size/formats/
    /// samples and no recreate is needed.
    [[nodiscard]] bool matches(uint32_t w, uint32_t h, sg_pixel_format color_fmt,
                               sg_pixel_format depth_fmt, int samples) const;

    /// Build the `sg_attachments` value the scene pass should use. `sg_attachments`
    /// is a struct (not a separately-created handle) — this is a convenience.
    [[nodiscard]] sg_attachments passAttachments() const;
};

} // namespace nodehammer::viewer
