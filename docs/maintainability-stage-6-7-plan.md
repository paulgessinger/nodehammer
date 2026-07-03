# Maintainability follow-up: Stage 6 + Stage 7 roadmap

Overview and sequencing for the two remaining items from the 2026-07
maintainability review, based on `maintainability-1-6`. The full design detail
for each lives in its own guide:

- **Stage 7** — [build-pipeline-dedup.md](build-pipeline-dedup.md)
- **Rest of Stage 6** — [viewer-app-decomposition.md](viewer-app-decomposition.md)

This document is the top-level roadmap: what the two efforts are, the decisions
that shape them, the order to land them in, and how much is machine-verifiable.

## Status

Implemented. All items landed: Stage 7 (`BuildPipeline` core + the four
call-site rewires) and the rest of Stage 6 (`DynamicRenderScale`, `IblBaker`,
`PngExporter`, `BuildController`). The CI-verifiable pieces are covered by unit
tests — `tests/tessellation/test_build_pipeline.cpp` (7a/7e, six cases) and
`tests/viewer/test_dynamic_render_scale.cpp` (6f) — and the full suite passes.
The viewer-coupled pieces (7b native, 6g/6h/6i) were compiled *and* run-verified
in a native D3D11 viewer build: loading the ODD through the reworked
`SceneBuildJob`/`BuildPipeline`, applying an angle cut, tessellating (1299 nodes
/ 1063 meshes), rendering, and driving the export state machine end-to-end. The
web rewires (7c/7d) are Emscripten-only and reviewed against their native twins.

---

## Context

The `maintainability-1-6` branch landed 8 standalone-PR commits from the review.
Two items were deliberately left open:

- **Rest of Stage 6** — commit `0412382` took only a *bounded* slice of the
  `app.cpp` god object (`jobsRunning()`, `iblDirty()`,
  `clampRenderScaleToMemoryBudget()`). The full decomposition of `render()`
  (~476 lines), `onFrame()` (~477), and the ~122-member `App::Impl` was deferred
  because the native viewer can't be linked/run in the CI sandbox (GTK/DBus +
  GL-loader gaps) — it needs a run-testable environment.
- **Stage 7** — the `prep → wedge → tessellate` build sequence is hand-copied at
  **4 sites** (native thread / web-cooperative / web-worker / synchronous). It
  was deferred "to design together."

## Decisions

1. **Base:** all work is off `maintainability-1-6` (not the parallel
   `docs/event-display-design`, which lacks commit 6).
2. **Stage 7 synchronous path:** *fully unify* it onto the shared `BuildPipeline`
   (eliminate the 4th copy), rather than keeping the one-shot path as a reference.
3. **Stage 7 cooperative backend:** *keep* it (and the `IWebBackend` seam and the
   mid-build replay) — it is the only fallback for `file://` / no-`Worker` / CSP /
   `?compute=main`, and after the dedup it thins to a ~20-line adapter over
   `BuildPipeline`. Stage 7 thins it; it does not remove it.
4. **Stage 6 delivery:** captured as design docs and implemented in a run-testable
   viewer environment; `DynamicRenderScale` is designed headless/unit-testable and
   lands first with tests.

---

## Stage 7 at a glance — collapse 4 copies onto one core primitive

Lift the sequence into a core-lib `BuildPipeline` (no viewer, no GPU → unit-testable).
The four current copies:

| Site | File / function |
|---|---|
| Native | `src/viewer/scene_build_job_native.cpp:81-120` |
| Web-cooperative | `src/viewer/scene_build_job_web.cpp:48-115` |
| Web-worker | `src/web/compute_worker_main.cpp:115-194` |
| Synchronous | `src/scene_build.cpp:54-105` |

Rewire order (each a PR): **7a** introduce `BuildPipeline` + unit tests (fully
CI-testable) → **7b** native → **7c** thin the cooperative backend → **7d**
web-worker → **7e** unify the synchronous path (fully CI-testable). Interface, the
five lockstep invariants, the phase-enum unification, the worker aliasing-`shared_ptr`
trick, and the six-test plan are in
[build-pipeline-dedup.md](build-pipeline-dedup.md).

## Rest of Stage 6 at a glance — four owned controllers

Break `App::Impl` into four controllers following the existing `Camera`/`BuildSession`
member + `UiActions`-callback pattern; `render()`/`onFrame()` become thin drivers.

| PR | Controller | Verifiable in CI? |
|---|---|---|
| 6f | `DynamicRenderScale` (adaptive render-scale) | **Yes** — designed headless + unit tests |
| 6g | `IblBaker` (debounce/bake/install) | No — GPU (`bakeIblGpu`) |
| 6h | `PngExporter` (export state machine) | No — GPU + Platform |
| 6i | `BuildController` (session + job + project) | No — rides on Stage 7's `SceneBuildJob` |

Order **6f → 6g → 6h → 6i**; land Stage 7 before 6i. State absorbed, interfaces, and
verification steps are in [viewer-app-decomposition.md](viewer-app-decomposition.md).

---

## Execution notes

- **Branches:** e.g. `stage-7-build-pipeline` and `stage-6-app-decomposition` off
  `maintainability-1-6`; each commit a standalone PR.
- **Sandbox build env:**
  `source /cvmfs/sft.cern.ch/lcg/views/LCG_109/x86_64-el9-gcc15-opt/setup.sh;
  unset PYTHONHOME PYTHONPATH; PATH=~/.local/bin:$PATH` (pipx conan 2.30). The
  viewer/wasm targets can't fully link here — see per-item verification in the detail
  docs.
- **What's CI-verifiable now:** Stage 7's `BuildPipeline` core (7a), the synchronous
  unify (7e), the pipeline parity/slicing tests, and `DynamicRenderScale` (6f). The
  native/web rewires (7b–7d) and the GPU-coupled controllers (6g/6h/6i) are executed
  and run-verified in a real viewer environment.
