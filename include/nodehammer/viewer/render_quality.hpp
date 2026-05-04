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

/// Runtime visual-quality controls for the viewer's render stack. Kept
/// separate from `Config` because Config is the user/CLI-facing persisted
/// surface; quality settings are runtime tunables that will grow as more
/// rendering features land. Most fields are no-ops today and only exist so
/// the UI plumbing path is established before HDR/FXAA/MSAA/etc. are wired.
struct RenderQualitySettings {
    float render_scale{1.0f};
    bool enable_hdr{false};
    bool enable_tonemap{false};
    bool enable_fxaa{false};
    bool enable_ao{false};
    float ao_intensity{1.0f};
    float ao_radius{0.3f};
    /// Multiplier on `ao_radius` defining the maximum length of a
    /// horizon-sample offset from the center pixel before the sample is
    /// rejected as a different surface (kills silhouette fringe). 1.0 ≈
    /// "samples up to one radius away count"; smaller values are stricter.
    float ao_thickness{1.0f};
    int msaa_samples{1};
    int ibl_quality{1};
    bool enable_bloom{false};
    DebugView debug_view{DebugView::Off};
    float exposure_stops{0.0f};
    TonemapMode tonemap_mode{TonemapMode::ACES};

    /// When on, the composite pass samples the IBL prefilter cubemap as a
    /// fullscreen background on pixels that haven't been written by scene
    /// geometry (depth = clear value). Driven by the same Nishita/gradient
    /// bake the IBL specular reflections come from, so the on-screen sky
    /// matches the reflected sky.
    bool enable_background{false};
};

} // namespace nodehammer::viewer
