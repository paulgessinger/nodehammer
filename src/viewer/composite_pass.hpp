#pragma once

#include <nodehammer/viewer/render_quality.hpp>

#include <sokol_gfx.h>

namespace nodehammer::viewer {

class AoPass;
struct AoRenderTarget;
struct SceneRenderTarget;

/// Fullscreen passthrough composite of the offscreen scene target into the
/// active swapchain pass. Optionally renders a depth debug view selected by
/// `DebugView`. No depth, no cull, no vertex buffer — issues sg_draw(0,3,1)
/// against a VS that synthesises a fullscreen triangle from gl_VertexIndex.
class CompositePass {
  public:
    CompositePass();
    ~CompositePass();
    CompositePass(const CompositePass &) = delete;
    CompositePass &operator=(const CompositePass &) = delete;

    /// Build the shader + pipeline. Must be called once after sg_setup.
    /// Idempotent.
    void initialize();

    /// Release the shader + pipeline. Must be called before sg_shutdown.
    /// Idempotent.
    void release();

    /// Issue the composite draw inside the currently-bound pass. The caller
    /// supplies camera near/far so the linearized-depth view can map to a
    /// uniform [0,1] gradient. `quality` carries the debug-view selector and
    /// FXAA / AO toggles; future composite-FS features (bloom, dither) plug
    /// in here without further signature churn.
    ///
    /// `ao_target` and `ao_pass` provide the AO map binding. When
    /// `quality.enable_ao` is false, the composite samples `ao_pass`'s 1×1
    /// white dummy so the pipeline's binding contract stays valid without a
    /// second pipeline variant.
    void draw(const SceneRenderTarget &scene_target, const AoRenderTarget &ao_target,
              const AoPass &ao_pass, const RenderQualitySettings &quality, float near_plane,
              float far_plane);

  private:
    sg_shader shader_{};
    sg_pipeline pipeline_{};
    bool initialized_{false};
};

} // namespace nodehammer::viewer
