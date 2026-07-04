# Build-pipeline deduplication

Plan for collapsing the four hand-written copies of the
`prep → wedge → tessellate` build sequence onto a single reusable core
primitive, `BuildPipeline`.

This document is forward-looking: it describes a target shape and a
commit-by-commit implementation order, not an implementation record.

## Status

Implemented. The `prep → wedge → tessellate` sequence now lives once in the core
`BuildPipeline` (`include/nodehammer/tessellation/build_pipeline.hpp`,
`src/tessellation/build_pipeline.cpp`); the four former copies are thin adapters
over it — native (`scene_build_job_native.cpp`), web-cooperative
(`scene_build_job_web.cpp`, ~130 → ~20 lines), web-worker
(`compute_worker_main.cpp`), and the synchronous `buildSceneFromPaths`. The
`SceneBuildJob::Phase` enum is now an alias of `BuildPipeline::Phase`, and the
worker's numeric wire codes are pinned to it with `static_assert`s. Covered by
`tests/tessellation/test_build_pipeline.cpp` (the six cases below) and
run-verified on a native viewer build against the ODD. This was "step 7" of the
2026-07 maintainability review.

---

## The problem

The same three-stage build — validate/select/dedup (**prep**), optional azimuthal
**wedge** cut, then **tessellate** to a `RenderScene` — is reimplemented, by hand, at
four sites. The *primitives* are already shared; only the *sequencing* is copied:

- **Prep** — [`prepareSceneForTessellationFromInputs`](../src/scene_build.cpp)
  (`src/scene_build.cpp:17`): validate → select → dedup, with an optional inline wedge.
- **Wedge** — cooperative [`WedgeCutJob`](../include/nodehammer/tessellation/wedge_cut.hpp)
  (`wedge_cut.hpp:79`), or the drive-to-completion `applyWedgeCut` shim.
- **Tessellate** — cooperative
  [`TessellationJob`](../include/nodehammer/tessellation/tessellation_job.hpp)
  (`tessellation_job.hpp:19`), or the one-shot `TessellationPass::lower`.

The four copies of the *sequence*:

| Site | File / function | Driver |
|---|---|---|
| Native | `src/viewer/scene_build_job_native.cpp:81-120` (worker lambda) | `std::thread`; `advance(UINT64_MAX)` spin |
| Web-cooperative | `src/viewer/scene_build_job_web.cpp:48-115` (`CooperativeBackend::poll`) | frame-sliced state machine; `advance(budget_ns)` |
| Web-worker | `src/web/compute_worker_main.cpp:115-194` (`nh_compute_build`) | 2nd wasm heap; `advance(kSliceNs)` throttles postMessage |
| Synchronous | `src/scene_build.cpp:54-105` (`buildSceneFromPaths`) | one-shot `lower()`; wedge inline in prep |

`CooperativeBackend::poll` is already the canonical state machine; the native and
web-worker sites are structural twins of it. The synchronous site is the odd one out —
it applies the wedge *inline* in prep and calls `TessellationPass::lower` directly
instead of driving the cooperative jobs.

### The five lockstep invariants

These must stay identical across every copy; today they are enforced only by discipline:

1. **Deferred wedge.** Async sites pass `std::nullopt` to prep and run the wedge as a
   *separate* `WedgeCutJob`, so its progress is reportable and re-cuts re-derive from
   pristine geometry. The synchronous site does the opposite (inline) — this refactor
   removes that divergence.
2. **Phase ordering.** `Preparing → Cutting → Tessellating → Finalizing`, and the numeric
   worker codes `kPreparing=1 … kFinalizing=4` (`compute_worker_main.cpp:48`), must match
   [`SceneBuildJob::Phase`](../src/viewer/scene_build_job.hpp) (`scene_build_job.hpp:78`)
   and the JS label map (`src/web/compute_worker.js`).
3. **Progress-counter semantics.** `wedgeCut{Total,Processed}` counts *placements*;
   `tessellation{Total,Processed}` counts *nodes*. The build-progress toast in `app.cpp`
   reads these uniformly across all backends.
4. **Result-packaging tail.** `prep.diags.append(tess.diags)` → `hasErrors()` gate →
   `make_shared<RenderScene>` must be equivalent at every site (see
   `scene_build_job_web.cpp:100-112`).
5. **Pristine-reuse contract.** Prep copies its inputs so the pristine scene is never
   mutated; native and the worker re-aim the wedge from uncut geometry each build.

---

## The fix: one core primitive

Lift the `CooperativeBackend` state machine into the **core library**. It depends only on
`prepareSceneForTessellationFromInputs`, `WedgeCutJob`, `TessellationJob`,
`TessellationPass`, and `SceneBuildResult` — all core, no viewer, no GPU — so it is
**fully unit-testable** in the standard build, unlike the viewer sites.

New files:

- `include/nodehammer/tessellation/build_pipeline.hpp`
- `src/tessellation/build_pipeline.cpp`

### Interface

Mirrors the `SceneBuildJob` surface so each backend collapses to a thin adapter:

```cpp
namespace nodehammer {

class BuildPipeline {
  public:
    // Canonical phase enum — the single source of truth for invariant #2.
    enum class Phase : std::uint8_t {
        Idle, Queued, Preparing, Cutting, Tessellating, Finalizing, Done
    };

    BuildPipeline();
    ~BuildPipeline();
    BuildPipeline(BuildPipeline &&) noexcept;
    BuildPipeline &operator=(BuildPipeline &&) noexcept;

    // Inputs held as shared_ptr<const>; the deep copy prep consumes is taken lazily on
    // the first advance() — this preserves the native "copy off the main thread" and the
    // cooperative "burn one paint frame before working" behaviours. The wedge is always
    // deferred to a WedgeCutJob (invariant #1).
    void start(std::shared_ptr<const NHConfig> config,
               std::shared_ptr<const SemanticScene> scene,
               std::optional<WedgeCutParams> wedgeCut = std::nullopt);

    // Advance one slice. Returns true once the build is complete. The first call only
    // transitions Queued → Preparing (returns false) so a frame-driven caller's last
    // frame paints; drive-to-completion callers just spin one extra no-op iteration.
    bool advance(std::uint64_t budget_ns = 8'000'000);

    // Move out the result. Valid only after advance() has returned true. Resets to Idle.
    SceneBuildResult take();

    [[nodiscard]] Phase phase() const;
    [[nodiscard]] std::size_t tessellationTotal() const;
    [[nodiscard]] std::size_t tessellationProcessed() const;
    [[nodiscard]] std::size_t wedgeCutTotal() const;
    [[nodiscard]] std::size_t wedgeCutProcessed() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer
```

### Implementation

The body is a near-verbatim lift of `CooperativeBackend::poll` (`scene_build_job_web.cpp:48-115`):
the `Queued → PrepPending → (Cutting?) → Tessellating → Finalizing → Done` machine,
including the invariant-#4 packaging tail (`web.cpp:100-112`) and the invariant-#1
`nullopt` prep (`web.cpp:61-62`). Move `logPreBuild` in as-is if the log line is wanted at
the core layer; otherwise leave logging to the backends.

### Phase-enum unification (removes invariant-#2 drift)

- In `scene_build_job.hpp`, replace the local enum with an alias:
  `using Phase = ::nodehammer::BuildPipeline::Phase;`. All existing UI references
  (`SceneBuildJob::Phase::Cutting`, …) keep compiling.
- Keep the worker's numeric protocol (it crosses a JS boundary) but add `static_assert`s
  in `compute_worker_main.cpp` binding each code to the enum:

  ```cpp
  static_assert(kPreparing    == static_cast<int>(BuildPipeline::Phase::Preparing));
  static_assert(kCutting      == static_cast<int>(BuildPipeline::Phase::Cutting));
  static_assert(kTessellating == static_cast<int>(BuildPipeline::Phase::Tessellating));
  static_assert(kFinalizing   == static_cast<int>(BuildPipeline::Phase::Finalizing));
  ```

  Any future reorder becomes a compile error instead of a silent UI mislabel.

### Worker cache: non-owning aliasing `shared_ptr` (keeps invariant #5)

`nh_compute_build` caches the pristine scene/config across wedge re-aims and must not hand
ownership to the pipeline. Wrap the cache with the aliasing `shared_ptr` constructor:

```cpp
ComputeCache &c = cache();
auto cfg = std::shared_ptr<const NHConfig>(std::shared_ptr<void>{}, &c.config);
auto scn = std::shared_ptr<const SemanticScene>(std::shared_ptr<void>{}, &c.scene);
BuildPipeline pipe;
pipe.start(cfg, scn, has_wedge ? std::optional{params} : std::nullopt);
```

Prep copies by value internally and never mutates the pointee, so the cache stays pristine
for the next re-aim.

---

## Implementation order (each item is one commit / PR)

### 7a — introduce `BuildPipeline` in core + unit tests
Pure addition; no call site changes yet. Register `src/tessellation/build_pipeline.cpp`
in the `nodehammer_lib` target (`CMakeLists.txt`). **Fully testable in the standard build.**

### 7b — rewire native (`scene_build_job_native.cpp`)
Replace the copied sequence in the worker lambda with:

```cpp
pipe.start(preset_config, preset_scene, wedge_cut);
while (!pipe.advance(std::numeric_limits<uint64_t>::max())) {}
result = pipe.take();
```

The pristine copy still happens on the worker thread (it is `start`+first-`advance`, both
inside the thread lambda). **Native phase wrinkle:** the UI reads `phase()` from the main
thread *during* the off-thread build. `BuildPipeline::phase()` is not thread-safe to read
concurrently, so keep the existing `std::atomic<Phase> worker_phase` mirror and update it
from the loop (`worker_phase.store(pipe.phase(), std::memory_order_relaxed)` each
iteration); the main-thread `phase()` reads the mirror, exactly as today. Compile-only in
the sandbox; run-verify on native.

### 7c — thin the web-cooperative backend (`scene_build_job_web.cpp`)
`CooperativeBackend` collapses from ~130 lines to ~20: hold a `BuildPipeline`, forward
`poll(budget) → pipe.advance(budget)`, and delegate every getter to the pipeline. Delete
the bespoke `State` enum (now owned by `BuildPipeline`).

> **Decision — keep the cooperative backend and the `IWebBackend` seam.** It is the only
> fallback for `file://`, no-`Worker`/CSP, and the `?compute=main` debug flag, and it
> backs the transparent mid-build replay in `SceneBuildJob::poll`
> (`scene_build_job_web.cpp:235-248`, guarded by `wantsFallback()`). After the dedup it
> costs ~20 lines to retain, so this refactor only *thins* it — it does **not** remove the
> backend, the strategy seam, or the replay path.

Emscripten target — build in a wasm environment.

### 7d — rewire the web-worker (`compute_worker_main.cpp`)
Replace the hand-rolled loop with the pipeline, keeping only the postMessage emission and
the serialization boundary:

```cpp
BuildPipeline pipe;
pipe.start(cfg, scn, wedge);              // aliasing shared_ptrs, see above
while (!pipe.advance(kSliceNs)) {
    emit_progress(pipe.phase(), processed_for(pipe), total_for(pipe));
}
SceneBuildResult r = pipe.take();
// package r.scene bytes exactly as today
```

`processed_for`/`total_for` pick the wedge vs tessellation counters off `pipe.phase()`.
Emscripten target.

### 7e — fully unify the synchronous path (`scene_build.cpp buildSceneFromPaths`)
Replace the one-shot `applyWedgeCut` + `TessellationPass::lower` with the pipeline driven
to completion:

```cpp
auto cfg = std::make_shared<const NHConfig>(std::move(prepInputsConfig));
auto scn = std::make_shared<const SemanticScene>(std::move(importResult.scene));
BuildPipeline pipe;
pipe.start(cfg, scn, wedge /* if the caller has one */);
while (!pipe.advance(std::numeric_limits<uint64_t>::max())) {}
return pipe.take();
```

This eliminates the 4th copy. Prep is now called with `nullopt` wedge (invariant-#1
alignment). **Parity risk:** the inline `applyWedgeCut` vs the `WedgeCutJob`
drive-to-completion must produce identical geometry — `wedge_cut.hpp:77` states
`applyWedgeCut` is *already* a thin shim over `WedgeCutJob`, so this is expected to be
semantically identical; test 2 below locks it in. **Fully testable in the standard build.**

---

## Tests

New: `tests/tessellation/test_build_pipeline.cpp`.

1. **Drive-to-completion parity.** `BuildPipeline` spun with `UINT64_MAX` yields a
   `RenderScene` equal to `TessellationPass::lower(prep.scene)` for a fixture scene
   (compare mesh/vertex/index counts and node→mesh mapping).
2. **Wedge parity (locks 7e).** Pipeline-with-wedge equals the pre-refactor
   `buildSceneFromPaths`-with-wedge output (shape counts + `WedgeCutStats`).
3. **Budget slicing.** With a tiny `budget_ns`, `advance()` returns false ≥ N times and the
   final scene is byte-identical to the drive-to-completion result.
4. **Phase & counter progression.** Phases advance in order; each `processed` reaches its
   `total`; counters are 0 before their phase.
5. **Error propagation (invariant #4).** A config that fails validation takes the `!ok`
   path → `scene == nullptr`, diags carry the error.
6. **Degenerate / absent wedge.** `std::nullopt` and a ≈0°/≈360° sector both skip `Cutting`
   cleanly and still produce the tessellated scene.

## Verification

- **Standard build:** `./build.sh cmake --build build && ./build.sh ctest --test-dir build -R build_pipeline`,
  plus the full suite for regression. Covers 7a, 7e, and all six tests.
- **Native (7b):** run the viewer, load the ODD, apply an angle cut; confirm the progress
  toast still shows Cutting → Tessellating and the final mesh is unchanged.
- **Web (7c/7d):** build the wasm targets; exercise both `?compute=main` (cooperative) and
  the default worker path; confirm the progress UI and the worker→cooperative fallback
  replay still work.
