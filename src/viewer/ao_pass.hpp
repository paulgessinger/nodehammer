#pragma once

#include <viewer/render_quality.hpp>

#include <sokol_gfx.h>

#include <cstdint>

namespace nodehammer::viewer {

struct Camera;
struct SceneRenderTarget;

/// GTAO pass — depth-only, full-resolution, single FS draw. Owns its shader,
/// pipeline, and a 1×1 R8 white dummy texture used by the composite pass when
/// AO is disabled (so the composite keeps a single pipeline variant with
/// stable bindings).
///
/// Caller pattern: open an sg_pass with an `AoRenderTarget`'s attachments,
/// then `draw(scene_rt, camera, w, h, quality)`, then `sg_end_pass()`.
class AoPass {
  public:
    AoPass();
    ~AoPass();
    AoPass(const AoPass &) = delete;
    AoPass &operator=(const AoPass &) = delete;

    /// Build the shader and the dummy 1×1 white R8 view+sampler. The
    /// pipeline itself is built lazily by `setTargetColorFormat` — sokol
    /// pipelines pin the color attachment format, so picking R16F vs R8 at
    /// AO-target creation time means the pipeline must be rebuilt to match.
    /// Idempotent.
    void initialize();

    /// Set the AO target's color format the pipeline should render into.
    /// Rebuilds the pipeline if the format changed; no-op otherwise. Must
    /// be called before `draw` (the App side calls it from the same place
    /// it allocates the target).
    void setTargetColorFormat(sg_pixel_format fmt);

    /// Release every sokol handle held. Idempotent.
    void release();

    /// Issue the GTAO draw inside the currently-bound pass. The depth view
    /// from the scene target is the only screen-space input. `target_w` and
    /// `target_h` are the AO target's dimensions (used to compute texel-size
    /// uniforms — must match the bound attachment).
    void draw(const SceneRenderTarget &scene_rt, const Camera &camera, uint32_t target_w,
              uint32_t target_h, const RenderQualitySettings &quality);

    /// 1×1 white texture view + nonfiltering sampler. Bound by the composite
    /// pass when AO is disabled so the pipeline's binding contract stays
    /// satisfied without a second pipeline variant.
    [[nodiscard]] sg_view dummyView() const { return dummy_view_; }
    [[nodiscard]] sg_sampler dummySampler() const { return dummy_sampler_; }

  private:
    sg_shader shader_{};
    sg_pipeline pipeline_{};
    sg_pixel_format current_color_format_{SG_PIXELFORMAT_NONE};
    sg_buffer vbuf_{};
    sg_image dummy_image_{};
    sg_view dummy_view_{};
    sg_sampler dummy_sampler_{};
    bool initialized_{false};
};

} // namespace nodehammer::viewer
