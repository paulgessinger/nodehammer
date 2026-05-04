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
    int msaa_samples{1};
    int ibl_quality{1};
    bool enable_bloom{false};
    DebugView debug_view{DebugView::Off};
    float exposure_stops{0.0f};
    TonemapMode tonemap_mode{TonemapMode::ACES};
};

} // namespace nodehammer::viewer
