#pragma once

#include <sokol_gfx.h>

namespace nodehammer::viewer {

/// Fullscreen 1:1 blit of a color texture into the currently-bound pass (the
/// swapchain). Backs the "pause when static" present-cache path: a full frame
/// renders the composite + ImGui into a persistent offscreen target, then this
/// blit copies it into the swapchain; paused frames re-run only the blit so the
/// last full frame is re-presented cheaply — and a valid frame is presented
/// every time, so sokol's unconditional D3D11 Present() never scans out a stale
/// flip-model back buffer (which read as the perf plot jumping backward).
///
/// Owns its shader, pipeline, a static fullscreen-triangle VBO, and a NEAREST
/// sampler so the copy is exact at a 1:1 pixel mapping. The pipeline targets the
/// swapchain's default color/depth format + sample count (like CompositePass),
/// so it must only be drawn inside a swapchain pass.
class BlitPass {
  public:
    BlitPass();
    ~BlitPass();
    BlitPass(const BlitPass &) = delete;
    BlitPass &operator=(const BlitPass &) = delete;

    /// Build the shader + pipeline + resources. Must be called once after
    /// sg_setup. Idempotent.
    void initialize();

    /// Release everything. Must be called before sg_shutdown. Idempotent.
    void release();

    /// Blit `src_view` (a sampled color texture view) full-screen into the
    /// currently-bound swapchain pass.
    void draw(sg_view src_view);

  private:
    sg_shader shader_{};
    sg_pipeline pipeline_{};
    sg_buffer vbuf_{};
    sg_sampler sampler_{};
    bool initialized_{false};
};

} // namespace nodehammer::viewer
