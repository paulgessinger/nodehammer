#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace nodehammer::viewer {

/// Max number of stamp() segments a single frame can record (scene / ao /
/// denoise / composite / export today — the rest is headroom).
inline constexpr int kGpuPassMaxSegments = 12;

/// One measured GPU span between two consecutive stamps: the label passed to the
/// stamp that *closed* the span, and its duration in milliseconds.
struct GpuPassSegment {
    char label[24]{};
    double ms{0.0};
};

/// The most recent *completed* frame's per-pass GPU times. Populated a few frames
/// after the work is submitted (timestamp queries are async), so it lags the live
/// frame by the ring depth — the same one-frame-late contract the CPU submit
/// timers already use in the Debug panel.
struct GpuPassTimings {
    std::array<GpuPassSegment, kGpuPassMaxSegments> segments{};
    int count{0};         ///< number of valid entries in `segments`
    double total_ms{0.0}; ///< GPU time from the first stamp to the last
    bool valid{false};    ///< false until a frame has been harvested (or if unsupported)
};

/// Per-pass GPU timing via backend timestamp queries.
///
/// The reason this exists: on D3D11 the frame's real stall is inside sokol_app's
/// `Present()`, which runs *after* the frame callback returns — so none of the
/// App's CPU-side submit timers (encode/present/gpu_wait, all measured up to
/// `sg_commit()`) can see the GPU cost. GPU timestamp queries measure the work on
/// the GPU timeline directly, regardless of where the CPU happens to block.
///
/// Usage, driven from App::Impl::render():
///   beginFrame();                 // marks the GPU-timeline t0
///   ... scene pass ...   stamp("scene");
///   ... ao pass ...      stamp("ao");      // only when the pass actually runs
///   ... composite ...    stamp("composite");
///   endFrame();                   // closes the frame; harvested a few frames later
///
/// Only the D3D11 backend implements real timing (see gpu_pass_timer_d3d11.cpp);
/// every other backend links the no-op TU where `enabled()` is false and
/// `results()` stays invalid. External GPU profilers (Nsight/PIX/RenderDoc) cover
/// the other backends, and Metal/GL/WebGPU don't exhibit the same
/// present-outside-instrumentation blind spot.
class GpuPassTimer {
  public:
    GpuPassTimer();
    ~GpuPassTimer();
    GpuPassTimer(const GpuPassTimer &) = delete;
    GpuPassTimer &operator=(const GpuPassTimer &) = delete;

    /// True once the backend timestamp-query machinery is live. False on
    /// backends without an implementation or if query creation failed.
    [[nodiscard]] bool enabled() const;

    /// Open a frame: reclaim the oldest ring slot (harvesting its result if
    /// ready), then mark the GPU-timeline start. No-op when disabled.
    void beginFrame();

    /// Record the GPU time elapsed since the previous stamp (or beginFrame).
    /// `label` names the span that just ended. Extra stamps past the segment
    /// cap are dropped. No-op when disabled or outside a begin/end frame.
    void stamp(const char *label);

    /// Close the frame. The result becomes available for harvest a few frames
    /// later. No-op when disabled.
    void endFrame();

    /// Most recently harvested completed frame's per-pass times. `valid` is
    /// false until one is available.
    [[nodiscard]] const GpuPassTimings &results() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
