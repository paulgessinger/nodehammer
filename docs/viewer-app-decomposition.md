# Viewer `App::Impl` decomposition

Plan for breaking the viewer's `App::Impl` god object into four owned
controller types, shrinking `render()` and `onFrame()` to thin drivers.

This document is forward-looking: it describes a target shape and a
commit-by-commit order, not an implementation record. It is the follow-on to
the *bounded* slice landed in the 2026-07 maintainability series (commit
`0412382`), which extracted `jobsRunning()`, `iblDirty()`, and
`clampRenderScaleToMemoryBudget()` and left the four large clusters inline.

## Status

Implemented. All four controllers landed —
[`DynamicRenderScale`](../src/viewer/dynamic_render_scale.cpp),
[`IblBaker`](../src/viewer/ibl_baker.cpp),
[`PngExporter`](../src/viewer/png_exporter.cpp), and
[`BuildController`](../src/viewer/build_controller.cpp) — each an owned member
of `App::Impl`, and `render()`/`onFrame()` shrank to thin drivers (`app.cpp`
dropped ~1000 lines). `DynamicRenderScale` is headless and covered by
`tests/viewer/test_dynamic_render_scale.cpp`; all four were compiled and
run-verified in a native D3D11 viewer build (load ODD → build/tessellate → angle
cut → render → export state machine, end to end). Line numbers below are against
the pre-refactor `maintainability-1-6` `src/viewer/app.cpp` (2440 lines).

---

## The problem

[`App::Impl`](../src/viewer/app.cpp) carries ~122 members. Two methods dominate:

- `App::Impl::render()` — `app.cpp:976-1452` (~476 lines).
- `App::Impl::onFrame()` — `app.cpp:1819-2296` (~477 lines).

The members cluster cleanly into four concerns, each already backed by sibling
helper translation units. Commit 6 factored out the three smallest predicates; the
four large clusters remain inline in these two methods.

## The pattern to follow (already in the codebase)

Extract each concern as an **owned controller member type** — exactly how
[`Camera`](../include/nodehammer/viewer/camera.hpp),
[`BuildSession`](../include/nodehammer/viewer/build_session.hpp), and
`Notifications` already live as members that `App::Impl` constructs and drives.

Effects a controller does not own are injected as `std::function` callbacks,
mirroring the `UiActions` inversion in
[`ui/ui_context.hpp`](../src/viewer/ui/ui_context.hpp) (`ui_context.hpp:34-53`),
where panels invoke intent and the App owns the implementation. Tunables stay in
the existing data-only structs
([`RenderQualitySettings`](../include/nodehammer/viewer/render_quality.hpp),
`IblSettings`, `PngExportSettings`).

This is a **mechanical move-state-plus-methods refactor, not a redesign**: no
behaviour change, no new rendering logic. Each controller gets its own
`{hpp,cpp}` under `src/viewer/`, becomes one `App::Impl` member, and the two god
methods shrink to thin drivers that call `controller.update(...)` /
`controller.poll(...)`.

---

## The four controllers

Each is one commit / PR. Order is deliberate: most-isolated and testable first;
`BuildController` last because it rides on the Stage 7 `SceneBuildJob` change
(see [build-pipeline-dedup.md](build-pipeline-dedup.md)).

### 6f — `DynamicRenderScale`  *(do first — the only unit-testable one)*

**Absorbs (state):** the `dyn_*` render-scale members, plus reads of
`fb_width`/`fb_height`.

**Absorbs (logic), all currently inline in `render()`:**
- camera motion detection (compare `camera` to the `dyn_prev_*` snapshot),
- the adaptive EMA controller (frame-time EMA, asymmetric drop-fast / climb-slow,
  hold/climb locks) — the bulk of the scale computation,
- the max-texture-size cap (clamp against `sg_query_limits().max_image_size_2d`),
- the already-extracted memory-budget clamp
  (`clampRenderScaleToMemoryBudget`, `app.cpp:921`), folded in as a private step.

**Interface (no sokol dependency — pass GPU limits in as plain data):**

```cpp
struct SgLimits { uint32_t max_image_size_2d; };          // filled from sg_query_limits()
struct CameraSnapshot { glm::dvec3 target; double yaw, pitch, distance, fov; int proj; };

class DynamicRenderScale {
  public:
    // Returns the render scale to use this frame; updates internal EMA/lock state.
    float update(const RenderQualitySettings &q, const CameraSnapshot &cam,
                 double frame_ms, uint32_t fb_w, uint32_t fb_h, const SgLimits &limits);
    [[nodiscard]] float current() const;
};
```

`render()` shrinks its whole scale block to
`const float render_scale = dyn_scale_.update(quality, snapshot(camera), delta_seconds*1000, fb_width, fb_height, {sg_query_limits().max_image_size_2d});`.

**Tests** (`tests/viewer/test_dynamic_render_scale.cpp`, headless): EMA converges toward
`render_scale_target_fps`; drop-fast/climb-slow asymmetry; clamps honour
`render_scale_min/max`, the texture cap, and the memory budget. First viewer logic that is
ctest-able. Add a small `nh_add_viewer_lib`-independent test target, or link the controller
TU into the existing test binary (it has no sokol/GPU deps).

### 6g — `IblBaker`

**Absorbs (state):** the `ibl_*` members and the `kIblRebakeDebounce` constant.

**Absorbs (logic):** the debounce → dirty-track → bake → install loop currently in
`onFrame()` (the IBL section near the top of the method), and `iblDirty()` (`app.cpp:436`).

**Interface:**

```cpp
class IblBaker {
  public:
    using InstallFn = std::function<void(const IblBakeData &)>;
    explicit IblBaker(InstallFn install);          // App wires install → both renderers

    void markDirty(const IblSettings &settings);   // called when UI edits settings
    void poll(std::chrono::steady_clock::time_point now);  // bakes when settle timer elapses
    [[nodiscard]] bool dirty() const;              // replaces iblDirty()
    [[nodiscard]] bool installed() const;
};
```

The App constructs it with
`IblBaker{[this](const IblBakeData &d){ scene_renderer.installIbl(d); cut_renderer.installIbl(d); ibl_installed = true; }}`.
`onFrame()` reduces to `ibl_baker_.poll(now);` plus `markDirty` from the settings-changed
path. GPU-coupled (`bakeIblGpu`, [`ibl.hpp`](../src/viewer/ibl.hpp) `:69`) → run-verified
only.

### 6h — `PngExporter`

**Absorbs (state):** the `export_*` and `startup_screenshot_*` members and the
`ExportPhase` enum (`app.cpp:200`).

**Absorbs (logic):** the five methods `requestPngExport` (`app.cpp:1454`), `exportPreRender`
(`1521`), `exportPostRender` (`1551`), `deliverExport` (`1612`), and `finishExport`
(`1631`) — the `Idle → Rendering → WaitGpu → Readback` state machine.

**Interface (hooks for the three things it doesn't own):**

```cpp
class PngExporter {
  public:
    struct Hooks {
        std::function<void()> render_one_frame;                    // into App::render()
        std::function<std::unique_ptr<ImageReadback>(uint32_t,uint32_t)> make_readback; // seam
        std::function<std::optional<std::string>(std::span<const uint8_t>)> deliver;     // → Platform::saveExportedImage
    };
    explicit PngExporter(Hooks hooks);

    void request(const PngExportSettings &s, std::string explicit_path, bool quit_when_done);
    void poll();                       // drives the phase machine once per frame
    [[nodiscard]] bool active() const;
    [[nodiscard]] ExportPhase phase() const;
};
```

The `ImageReadback` GPU seam ([`png_export_readback.hpp`](../src/viewer/png_export_readback.hpp)
`:30`) and the pure `downscaleBoxRgba8`/`encodePngRgba8` functions
([`png_export.hpp`](../include/nodehammer/viewer/png_export.hpp)) stay as-is; the exporter
just orchestrates them. GPU + Platform → run-verified only.

### 6i — `BuildController`  *(do last — rides on Stage 7's `SceneBuildJob`)*

**Absorbs (state):** the `build_*`, `cut_*`, `pristine_*`, and `root_*_key` members, plus
`jobsRunning()` (`app.cpp:427`).

**Absorbs (logic):** the build-drive block in `onFrame()` (poll `build_job`, route the
result to base vs cut scene, manage `build_progress_handle`/`build_error`), and the
project/build pipeline including the `start_build` lambda (walk → resolve → build → the
`pending_cut_rebuild` re-aim path).

**Interface:** wraps [`BuildSession`](../include/nodehammer/viewer/build_session.hpp) +
[`SceneBuildJob`](../src/viewer/scene_build_job.hpp) + `ProjectFs::generation()`. Emits
outcomes via callbacks the App binds to its scene/upload state:

```cpp
class BuildController {
  public:
    struct Callbacks {
        std::function<void(std::shared_ptr<RenderScene>)> on_base_scene_ready;
        std::function<void(std::shared_ptr<RenderScene>)> on_cut_scene_ready;
        std::function<void(std::string)> on_error;
    };
    void poll(ProjectFs *project);          // drive session + job each frame
    void requestCut(double start_deg, double end_deg);
    [[nodiscard]] bool jobsRunning() const; // replaces the free predicate
    [[nodiscard]] SceneBuildJob::Phase phase() const;
    // ...progress getters forwarded to SceneBuildJob for the toast
};
```

GPU-uploads stay in the App (`beginUpload`/`advanceUpload` in `render()`); the controller
owns only the CPU build orchestration. Run-verified.

---

## Result

After all four land, `render()` and `onFrame()` become orchestration shells (target: each
< ~150 lines): call the four controllers, run the offscreen passes, assemble the
`ViewerUiContext`. The `App::Impl` member count drops by the four clusters. The
`ViewerUiContext` assembly (currently reading loose members) reads the controllers'
getters instead.

## Ordering & verification

Order: **6f → 6g → 6h → 6i**. If Stage 7 is in flight, land it before 6i (which builds on
the reworked `SceneBuildJob`).

- **6f:** `./build.sh ctest --test-dir build -R dynamic_render_scale` + full suite.
- **6g / 6h / 6i:** compile the viewer objects in the sandbox (compile-only), then in a
  real viewer environment run the viewer and exercise, respectively: IBL rebake (drag a
  sun-direction slider and confirm the debounced re-bake), PNG export (`--screenshot` and
  the interactive path), and a project load + angle-cut rebuild. Confirm no visual or
  timing regression versus `maintainability-1-6` HEAD.

Because each controller is a self-contained member with a narrow interface, the four PRs
are independent and reviewable in isolation even though only 6f is machine-verifiable in
CI.
