#pragma once

namespace nodehammer::viewer {

enum class DebugView : int {
    Off = 0,
    Depth = 1,
    LinearDepth = 2,
};

enum class TonemapMode : int {
    ACES = 0,
    Reinhard = 1,
    AgX = 2,
};

/// GTAO sample-count preset. Drives (NUM_SLICES, NUM_STEPS) in the AO FS:
///   Low    = 4×3 = 12 taps per pixel  (cheap, more residual jitter)
///   Medium = 4×4 = 16 taps per pixel  (default; the previous hard-coded value)
///   High   = 6×6 = 36 taps per pixel  (visibly cleaner on uniform surfaces)
///   Ultra  = 8×8 = 64 taps per pixel  (diminishing returns; for hero screenshots)
/// The AO shader caps the loop bounds at the Ultra values (compile-time
/// constants for the runtime loops) and uses uniforms to bail early.
enum class AoQualityPreset : int {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
};

/// Runtime visual-quality controls for the viewer's render stack. Kept
/// separate from `Config` because Config is the user/CLI-facing persisted
/// surface; quality settings are runtime tunables that will grow as more
/// rendering features land. Most fields are no-ops today and only exist so
/// the UI plumbing path is established before HDR/FXAA/MSAA/etc. are wired.
struct RenderQualitySettings {
    float render_scale{1.0f};
    bool enable_hdr{true};
    bool enable_tonemap{true};
    bool enable_fxaa{true};
    bool enable_ao{true};
    float ao_intensity{1.0f};
    float ao_radius{0.3f};
    /// Multiplier on `ao_radius` defining the maximum length of a
    /// horizon-sample offset from the center pixel before the sample is
    /// rejected as a different surface (kills silhouette fringe). 1.0 ≈
    /// "samples up to one radius away count"; smaller values are stricter.
    float ao_thickness{1.0f};
    /// Drives the GTAO sample count. More samples = less per-pixel jitter
    /// (and less load on the bilateral denoise downstream) at the obvious
    /// linear cost. Defaults to Medium = 4×4 = 16 taps per pixel, which is
    /// where the original implementation was hard-coded.
    AoQualityPreset ao_quality{AoQualityPreset::Medium};
    /// Run the bilateral denoise pass on the raw GTAO output. Strict
    /// quality win when on — exposed as a toggle so you can A/B raw GTAO
    /// jitter vs the denoised version without rebuilding. Off = scene
    /// shader / composite sample the raw target directly.
    bool enable_ao_denoise{true};
    /// When on: the scene shader's PBR branch consumes AO with the full
    /// layered treatment — multi-bounce diffuse, bent-normal-biased
    /// irradiance lookup, and specular occlusion on the IBL specular
    /// term. When off: composite does the legacy single-multiply against
    /// the denoised AO scalar (no bent normals, no multi-bounce, no SO).
    /// Toggle is the cleanest A/B for evaluating the layered features.
    bool enable_advanced_ao{true};
    /// Blend the GTAO bent normal toward the geometric surface normal.
    ///   0.0 = use N (no bent-normal effect on the irradiance lookup)
    ///   1.0 = use raw bent normal (full effect; can be noisy on uniform
    ///         surfaces and biases the IBL toward V somewhat because of
    ///         the per-slice averaging).
    /// Default 0.5 is a balance — most of the "cavities feel grounded"
    /// win without the noisier extremes. Only consulted when
    /// `enable_advanced_ao` is on.
    float ao_bent_strength{0.5f};
    int msaa_samples{1};
    int ibl_quality{1};
    bool enable_bloom{false};
    DebugView debug_view{DebugView::Off};
    float exposure_stops{1.3f};
    /// AgX has more contrast than the Narkowicz ACES fit at neutral
    /// exposure, so diffuse-lit interior scenes (typical detector views)
    /// look less flat out of the box. Users can switch back via the UI.
    TonemapMode tonemap_mode{TonemapMode::AgX};
    /// Pre-tonemap "look" knobs applied in linear HDR space, after
    /// exposure and before the tonemap curve. Defaults are no-ops.
    /// Contrast pivots around perceptual mid-gray (0.18); >1 increases
    /// contrast, <1 flattens. Saturation lerps between rec.709 luma and
    /// color; 0 = grayscale, 1 = identity, >1 boosts.
    float contrast{1.9f};
    float saturation{1.0f};

    /// When on, the composite pass samples the IBL prefilter cubemap as a
    /// fullscreen background on pixels that haven't been written by scene
    /// geometry (depth = clear value). Driven by the same Nishita/gradient
    /// bake the IBL specular reflections come from, so the on-screen sky
    /// matches the reflected sky.
    bool enable_background{true};
};

} // namespace nodehammer::viewer
