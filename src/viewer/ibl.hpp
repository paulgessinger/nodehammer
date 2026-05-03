#pragma once

#include <sokol_gfx.h>

namespace nodehammer::viewer {

/// Result of a GPU IBL bake. The three images are owned by the caller after
/// `bakeIblGpu()` returns and must eventually be released (typically via
/// `IblResources::release()` once installed).
struct IblBakeData {
    static constexpr int kIrradianceSize = 32;
    static constexpr int kPrefilterSize = 128;
    static constexpr int kPrefilterMips = 6; // 128, 64, 32, 16, 8, 4
    static constexpr int kBrdfLutSize = 256;

    sg_image brdf_lut{};   ///< 2D, 256x256, RG packed as RGBA8
    sg_image irradiance{}; ///< Cube, 32x32x6, no mips
    sg_image prefilter{};  ///< Cube, 128x128x6, 6 mips
    int prefilter_mip_count{kPrefilterMips};
};

/// Run all IBL bake passes synchronously and return the resulting GPU images.
/// Must be called inside a sokol frame (between sg_begin_pass cycles, before
/// any pass that samples from the result). Issues:
///   - 1 pass for the 256x256 BRDF LUT
///   - 6 passes for irradiance (one per cube face)
///   - 36 passes for prefilter (6 faces x 6 mips)
[[nodiscard]] IblBakeData bakeIblGpu();

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
    /// before the real bake runs. Caller must `release()` first to swap.
    void createDummy();

    /// Take ownership of GPU images produced by `bakeIblGpu()` and create
    /// the sample views + samplers needed by the scene shader. Caller must
    /// `release()` first to swap an existing set out.
    void upload(const IblBakeData &data);

    /// Destroy all sokol handles. Idempotent.
    void release();
};

} // namespace nodehammer::viewer
