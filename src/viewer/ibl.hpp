#pragma once

#include <sokol_gfx.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace nodehammer::viewer {

/// CPU-side baked IBL byte buffers, ready for sokol upload.
/// All textures are RGBA8 (LDR-clamped) so they work on every sokol backend
/// without floating-point storage support.
struct IblBakeData {
    static constexpr int kIrradianceSize = 32;
    static constexpr int kPrefilterSize = 128;
    static constexpr int kPrefilterMips = 6; // 128, 64, 32, 16, 8, 4
    static constexpr int kBrdfLutSize = 256;

    std::vector<std::byte> brdf_lut;            ///< 256x256 RG packed as RGBA8
    std::vector<std::byte> irradiance_combined; ///< cube, 6 faces packed
    std::array<std::vector<std::byte>, kPrefilterMips>
        prefilter_mips; ///< cube per mip, 6 faces packed
};

/// Synchronous full bake. Pure CPU; no sokol calls. Used directly on
/// platforms that bake on a worker thread; `IblBakeJob` drives a chunked
/// version of the same work on platforms without threads.
[[nodiscard]] IblBakeData bakeIbl();

/// Pre-baked image-based lighting resources for the procedural sky
/// environment.
struct IblResources {
    sg_image irradiance{};     ///< Cube, 32x32x6, no mips. Diffuse term.
    sg_image prefilter{};      ///< Cube, 128x128x6, 6 mips. Specular split-sum.
    sg_image brdf_lut{};       ///< 2D, 256x256, RG channels packed as RGBA8.
    sg_view irradiance_view{}; ///< texture-view of irradiance for sg_bindings
    sg_view prefilter_view{};
    sg_view brdf_lut_view{};
    sg_sampler cube_sampler{}; ///< trilinear, clamp_to_edge, mipmap_linear
    sg_sampler lut_sampler{};  ///< linear, clamp_to_edge

    /// Mip count of the prefilter cubemap (used as the max-LOD value for
    /// roughness sampling in the shader).
    int prefilter_mip_count{0};

    /// Create 1x1 placeholder textures + samplers so rendering can proceed
    /// before the real bake finishes. Caller must `release()` first to
    /// swap.
    void createDummy();

    /// Upload a baked dataset to the GPU. Caller must `release()` first to
    /// swap an existing set out.
    void upload(const IblBakeData &data);

    /// Destroy all sokol handles. Idempotent.
    void release();
};

/// Drives the IBL bake to completion across one or more frames. On
/// platforms with std::thread the bake runs on a worker and `advance` just
/// polls a flag. On emscripten (no pthreads) `advance` runs the bake
/// incrementally on the main thread, doing as much work as fits within
/// `budget_ns`.
class IblBakeJob {
  public:
    IblBakeJob();
    ~IblBakeJob();
    IblBakeJob(const IblBakeJob &) = delete;
    IblBakeJob &operator=(const IblBakeJob &) = delete;

    /// Begin the bake. Idempotent — second call is a no-op.
    void start();

    /// Make progress. Returns true once the bake is fully complete; the
    /// caller can then `take()` the result. `budget_ns` is ignored on
    /// platforms with a real worker thread.
    bool advance(uint64_t budget_ns = 8'000'000);

    /// Move out the baked data. Valid only after `advance()` returns true.
    [[nodiscard]] IblBakeData take();

    /// Fraction of the bake completed so far, in [0, 1]. Computed from the
    /// pixel iterator on web; on native (worker-thread bake) returns 0.0
    /// while the thread is running and 1.0 once it has signalled done.
    /// Pixel costs aren't uniform across stages — this is a rough monotonic
    /// signal for UI feedback, not a precise time estimate.
    [[nodiscard]] double progress() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
