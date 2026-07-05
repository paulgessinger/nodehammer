#pragma once

namespace nodehammer::viewer {

enum class DebugView : int {
    Off = 0,
    Depth = 1,
    LinearDepth = 2,
    /// Overdraw heatmap: re-renders the scene with additive blending and no
    /// depth test so each pixel accumulates the number of fragments that
    /// cover it, then the composite maps that count through a jet ramp.
    /// Diagnoses where geometry stacks up in depth (dense calorimeter /
    /// tracker plane stacks) -- the density that drives moire aliasing.
    Overdraw = 3,
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

/// FXAA edge-search quality. Caps how many steps the PC-quality edge walk
/// takes before giving up; longer walks resolve longer near-axis edges at
/// linear cost. The FS loops to the Ultra step count (a compile-time bound)
/// and bails early via a uniform, mirroring the GTAO loop approach.
///   Low = 4, Medium = 8, High = 10, Ultra = 12 search steps.
enum class FxaaQualityPreset : int {
    Low = 0,
    Medium = 1,
    High = 2,
    Ultra = 3,
};

/// Runtime visual-quality controls for the viewer's render stack. Kept
/// separate from `Config` because Config is the user/CLI-facing persisted
/// surface; quality settings are runtime tunables that grow as more
/// rendering features land. HDR, tonemap, AO, FXAA, render scale and the
/// "look" knobs are live.
struct RenderQualitySettings {
    float render_scale{1.0f};
    /// Dynamic render scale: when on (the default), the offscreen scale drops to
    /// `render_scale_min` while the camera is moving and jumps up to
    /// `render_scale_max` once it settles — trading transient sharpness for a
    /// steady framerate during interaction, then a high-fidelity still. Paired
    /// with `pause_when_static`, the expensive settled frame renders once and is
    /// cached. When off, the static `render_scale` above is used throughout.
    bool dynamic_render_scale{false};
    /// When dynamic scaling is on, run a closed-loop controller while the camera
    /// moves: hold the highest scale in [render_scale_min, render_scale_max]
    /// that meets `render_scale_target_fps`. Off = always render at
    /// render_scale_min while moving (a fixed in-motion resolution rather than a
    /// floor). Settled always jumps to render_scale_max regardless.
    bool adaptive_render_scale{true};
    /// Frame-time budget for the adaptive controller, in frames per second.
    float render_scale_target_fps{60.0f};
    /// Lowest scale used while the camera moves. With adaptive scaling on it's
    /// the floor the controller won't drop below; with it off it's the fixed
    /// in-motion resolution.
    float render_scale_min{0.66f};
    /// Resolution the scale jumps to once the camera settles (quality target).
    /// 1.0 = native; >1 supersamples the still image (up to 4x). The transition
    /// is a single step, not a ramp through intermediate resolutions — stepping
    /// reallocates the offscreen targets and resets the AO history each time,
    /// which reads as flicker.
    float render_scale_max{4.0f};
    /// Memory budget (in MB) for the resolution-scaling offscreen targets: the
    /// scene color + depth, plus the two AO targets (at ao_resolution_scale² of
    /// the scene area) when AO is on. The effective maximum render scale is
    /// lowered so these targets fit the budget — this keeps a large window times
    /// a high `render_scale_max` (up to 4x = 16x the pixels) from OOMing
    /// memory-constrained backends (WebGL/WebGPU, mobile, integrated GPUs). The
    /// derived cap never drops below 0.25x; 0 disables the budget cap entirely.
    /// Fixed-size resources (IBL cubemaps, swapchain) are not counted.
    /// Defaults are platform-keyed: 2 GB on native (discrete-GPU headroom),
    /// 1 GB on web where the browser/WebGL context is tighter.
#ifdef __EMSCRIPTEN__
    float render_scale_memory_budget_mb{1024.0f};
#else
    float render_scale_memory_budget_mb{2048.0f};
#endif
    /// Cap the render rate at 60 FPS. sokol drives the frame callback at the
    /// display's vsync rate; on a high-refresh panel (120Hz+) this skips frames
    /// to hold the visible rate at ~60. Off = render at the full refresh rate.
    bool cap_fps{false};
    /// Render the scene on demand. The cheap composite + ImGui run on every
    /// frame the loop ticks, so the UI stays live (hover, panels, graphs), but
    /// the expensive scene + AO passes only re-run when the view actually
    /// changes (camera, a held widget such as the wedge cut, a build/bake job)
    /// plus a short settle window so AO and the dynamic-scale jump converge.
    /// Moving the cursor or working the panels reuses the cached scene instead
    /// of re-rendering geometry. On top of that, when nothing is changing the
    /// loop caps to a low idle rate (~12 Hz) to trim power — full refresh
    /// resumes the instant anything happens, so interaction is unaffected. Most
    /// valuable with an expensive supersampled settled frame.
    bool pause_when_static{true};
    bool enable_hdr{true};
    bool enable_tonemap{true};
    /// Material-stack prefilter: band-limit the cycling-material pattern on
    /// merged sampling stacks (calorimeter absorber/scintillator layers) by
    /// blending each stack mesh's albedo toward its area-weighted average as
    /// the pixel footprint outgrows the band width -- kills the moire those
    /// thin high-contrast layers produce at distance. Only affects meshes the
    /// tessellation pass tagged with a StackAverage; a no-op elsewhere.
    bool enable_material_prefilter{true};
    /// Scales the stack-prefilter transition: the blend toward the average
    /// completes once the pixel footprint reaches `scale * featureSize` band
    /// widths. >1 keeps the crisp bands to a closer distance (blend later);
    /// <1 blends earlier/more aggressively. A global dial on top of the
    /// per-stack feature size baked at tessellation time.
    float material_prefilter_scale{1.0f};
    bool enable_fxaa{true};
    /// FXAA (PC-quality variant) tunables. Only consulted when enable_fxaa.
    /// Sub-pixel aliasing removal: blends the pixel toward the local 3×3
    /// lowpass. 0 = edge-only AA (sharpest, but thin features can still
    /// shimmer); 1 = maximum softening. 0.75 is the classic FXAA default.
    float fxaa_subpix{0.75f};
    /// Minimum local luma contrast (relative to the brighter neighbor) for a
    /// pixel to count as an edge. Lower = catch fainter edges (smoother, can
    /// soften texture); higher = only strong edges. FXAA "quality" default is
    /// ~0.166 (1/6); 0.125 is a touch more aggressive.
    float fxaa_edge_threshold{0.166f};
    /// Absolute luma floor below which edges are ignored as noise — keeps FXAA
    /// off near-black regions where relative contrast is meaningless.
    float fxaa_edge_threshold_min{0.0833f};
    /// Edge-search step budget — see FxaaQualityPreset.
    FxaaQualityPreset fxaa_quality{FxaaQualityPreset::High};
    bool enable_ao{true};
    float ao_intensity{1.8f};
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
    AoQualityPreset ao_quality{AoQualityPreset::Ultra};
    /// Fraction of the scene resolution at which the GTAO + denoise passes
    /// render. AO is low-frequency, so half-res (0.5 → ¼ the pixels) reclaims
    /// most of the fullscreen AO cost for little visible loss; the result is
    /// bilinearly upsampled when the scene shader / composite sample it.
    /// 1.0 = full res. Independent of (and multiplied on top of) render_scale.
    float ao_resolution_scale{1.0f};
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
    DebugView debug_view{DebugView::Off};
    /// Overdraw heatmap (DebugView::Overdraw) scale: the per-pixel fragment
    /// count that maps to the hot end of the ramp. Pixels covered more than
    /// this many times clamp to white to flag the worst hotspots. Raise it
    /// for very dense scenes (calorimeter / tracker stacks) where the
    /// interesting depth-complexity range runs higher.
    float overdraw_range{16.0f};
    float exposure_stops{1.6f};
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
