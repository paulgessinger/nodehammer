#pragma once

#include <viewer/render_quality.hpp>

#include <sokol_gfx.h>

#include <cstdint>

namespace nodehammer::viewer {

struct AoRenderTarget;
struct SceneRenderTarget;
struct Camera;

/// Depth-aware bilateral denoise on the GTAO raw output. Reads the raw AO
/// target (RGBA8: AO in R, octahedral bent normal in GB) plus scene depth,
/// writes a denoised target with the same layout.
///
/// Owns its shader, pipeline, and a static fullscreen-triangle VBO. The
/// pipeline color format is set lazily by `setTargetColorFormat` (same
/// pattern as `AoPass`) so the App side can pick RGBA8 once and reuse the
/// shader for both raw and history targets.
///
/// Caller pattern: open an sg_pass with the denoised target's attachments,
/// then `draw(raw_target, scene_rt, camera)`, then `sg_end_pass()`.
class AoDenoisePass {
  public:
    AoDenoisePass();
    ~AoDenoisePass();
    AoDenoisePass(const AoDenoisePass &) = delete;
    AoDenoisePass &operator=(const AoDenoisePass &) = delete;

    /// Build the shader (pipeline deferred to `setTargetColorFormat` so the
    /// color attachment format is pinned correctly). Idempotent.
    void initialize();

    /// Set the color format the denoise pipeline renders into (matches the
    /// AO history target's format — typically RGBA8). Rebuilds the pipeline
    /// when the format changes; no-op otherwise. Must be called before
    /// `draw`.
    void setTargetColorFormat(sg_pixel_format fmt);

    /// Release every sokol handle held. Idempotent.
    void release();

    /// Issue the denoise draw inside the currently-bound pass. Reads from
    /// `raw_ao` (the GTAO output) and `scene_rt.depth_texture_view` for the
    /// bilateral depth weight. `target_w` / `target_h` are the *output*
    /// target dimensions (used to derive texel size). `camera` supplies
    /// near/far for depth linearization.
    void draw(const AoRenderTarget &raw_ao, const SceneRenderTarget &scene_rt, const Camera &camera,
              uint32_t target_w, uint32_t target_h);

  private:
    sg_shader shader_{};
    sg_pipeline pipeline_{};
    sg_pixel_format current_color_format_{SG_PIXELFORMAT_NONE};
    sg_buffer vbuf_{};
    bool initialized_{false};
};

} // namespace nodehammer::viewer
