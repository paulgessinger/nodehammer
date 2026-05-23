#pragma once

#include "shared_image.hpp"

#include <glm/vec3.hpp>
#include <sokol_gfx.h>

namespace nodehammer::viewer {

/// Result of a GPU IBL bake. The three images are reference-counted, so the
/// bake can be installed into more than one `IblResources` (e.g. the base and
/// Boolean-cut renderers) and the GPU images are freed once the last holder
/// releases — no double-bake, no double-free.
struct IblBakeData {
    static constexpr int kIrradianceSize = 32;
    static constexpr int kPrefilterSize = 128;
    static constexpr int kPrefilterMips = 6; // 128, 64, 32, 16, 8, 4
    static constexpr int kBrdfLutSize = 256;

    SharedImage brdf_lut;   ///< 2D, 256x256, RG packed as RGBA8
    SharedImage irradiance; ///< Cube, 32x32x6, no mips
    SharedImage prefilter;  ///< Cube, 128x128x6, 6 mips
    int prefilter_mip_count{kPrefilterMips};
};

/// Procedural sky model used by the IBL bake. Nishita is single-scattering
/// Rayleigh+Mie atmospheric scattering — gives a real sun disc, horizon
/// warming, and turbidity-driven haze. Gradient is the legacy 3-stop vertical
/// ramp + soft sun spot, kept for fallback / comparison.
enum class SkyModel : int {
    Gradient = 0,
    Nishita = 1,
};

/// Tunable parameters fed to the bake shader at rebake time. Changing any
/// field invalidates the in-memory bake cache (operator== drives the
/// debounced rebake loop in `App::onFrame`).
struct IblSettings {
    int brdf_samples{1024};       ///< GGX importance samples per BRDF LUT pixel
    int irradiance_samples{1024}; ///< cosine-hemisphere samples per irradiance pixel
    int prefilter_samples{512};   ///< GGX importance samples per prefilter pixel

    SkyModel sky_model{SkyModel::Nishita};

    // Gradient model parameters. Unused under Nishita.
    glm::vec3 zenith_color{0.55f, 0.65f, 0.85f};
    glm::vec3 horizon_color{0.85f, 0.80f, 0.72f};
    glm::vec3 ground_color{0.20f, 0.18f, 0.16f};
    float sun_sharpness{64.0f}; ///< gradient model: disc exponent

    // Nishita model parameters.
    float turbidity{2.5f};                        ///< 1.5=clear, ~6=hazy. Drives Mie strength.
    glm::vec3 ground_albedo{0.08f, 0.08f, 0.08f}; ///< planet-surface reflectance

    // Shared between models (also used by the analytical scene light).
    glm::vec3 sun_dir{0.4f, 0.7f, 0.6f};     ///< toward-sun direction (normalized in shader)
    glm::vec3 sun_color{1.0f, 0.95f, 0.85f}; ///< sun tint (multiplied by sun_intensity)
    float sun_intensity{1.8f};               ///< scalar multiplier — pushes the disc into HDR

    bool operator==(const IblSettings &) const = default;
};

/// Run all IBL bake passes synchronously and return the resulting GPU images.
/// Must be called inside a sokol frame (between sg_begin_pass cycles, before
/// any pass that samples from the result). Issues:
///   - 1 pass for the 256x256 BRDF LUT
///   - 6 passes for irradiance (one per cube face)
///   - 36 passes for prefilter (6 faces x 6 mips)
[[nodiscard]] IblBakeData bakeIblGpu(const IblSettings &settings = {});

/// Pre-baked image-based lighting resources for the procedural sky
/// environment.
struct IblResources {
    // Images are reference-counted so an installed bake can be shared across
    // renderers; views and samplers are owned per-IblResources instance.
    SharedImage irradiance;    ///< Cube, 32x32x6, no mips. Diffuse term.
    SharedImage prefilter;     ///< Cube, 128x128x6, 6 mips. Specular split-sum.
    SharedImage brdf_lut;      ///< 2D, 256x256, RG channels packed as RGBA8.
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
