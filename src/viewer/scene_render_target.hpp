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
/// The depth convention is backend-conditional (see `useReversedZ` in
/// backend_caps.hpp): reversed-Z (depth-clear 0.0, GREATER_EQUAL) on
/// `[0,1]` clip-depth backends, normal-Z (depth-clear 1.0, LESS_EQUAL)
/// on GLES3. The caller's pass action and the scene pipeline must agree.
/// See scene_renderer.cpp.
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
    sg_pixel_format color_format{SG_PIXELFORMAT_RGBA8};
    sg_pixel_format depth_format{SG_PIXELFORMAT_DEPTH};

    /// Allocate (or reallocate, after destroy()) all resources for the given
    /// size/formats. The target is single-sampled — antialiasing is handled by
    /// render scale (SSAA) and the composite-pass FXAA, not MSAA. Must be
    /// called inside an active sokol_gfx session (between sg_setup/sg_shutdown).
    ///
    /// When `for_readback` is set, the color image is created so the PNG-export
    /// GPU→CPU readback can copy from it (via `makeReadbackColorImage` — only
    /// WebGPU needs the special CopySrc texture; other backends are unaffected).
    void create(uint32_t w, uint32_t h, sg_pixel_format color_fmt, sg_pixel_format depth_fmt,
                bool for_readback = false);

    /// Release every sokol handle held. Idempotent — safe to call when the
    /// target was never created or was already destroyed.
    void destroy();

    /// True when the existing resources match the requested size/formats and
    /// no recreate is needed.
    [[nodiscard]] bool matches(uint32_t w, uint32_t h, sg_pixel_format color_fmt,
                               sg_pixel_format depth_fmt) const;

    /// Build the `sg_attachments` value the scene pass should use. `sg_attachments`
    /// is a struct (not a separately-created handle) — this is a convenience.
    [[nodiscard]] sg_attachments passAttachments() const;
};

} // namespace nodehammer::viewer
