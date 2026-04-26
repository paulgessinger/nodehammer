#pragma once

#include <sokol_gfx.h>

namespace nodehammer::viewer {

/// Pre-baked image-based lighting resources for the procedural sky
/// environment. All textures are RGBA8 (LDR-clamped) so they work on every
/// sokol backend without floating-point storage support.
struct IblResources {
    sg_image irradiance{};     ///< Cube, 32x32x6, no mips. Diffuse term.
    sg_image prefilter{};      ///< Cube, 128x128x6, 6 mips. Specular split-sum.
    sg_image brdf_lut{};       ///< 2D, 256x256, RG channels packed as RGBA8.
    sg_view irradiance_view{}; ///< texture-view of irradiance for sg_bindings
    sg_view prefilter_view{};
    sg_view brdf_lut_view{};
    sg_sampler cube_sampler{}; ///< trilinear, clamp_to_edge, mipmap_linear
    sg_sampler lut_sampler{};  ///< linear, clamp_to_edge

    /// Bake all three textures from the procedural sky and upload to GPU.
    /// Must be called after sg_setup; idempotent (existing handles are kept
    /// unless `release` was called first).
    void create();

    /// Destroy all sokol handles. Idempotent.
    void release();

    /// Mip count of the prefilter cubemap (used as the max-LOD value for
    /// roughness sampling in the shader). Set by `create`.
    int prefilter_mip_count{0};
};

} // namespace nodehammer::viewer
