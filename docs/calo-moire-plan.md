# Calorimeter moiré: prefilter + hull-LOD plan / handoff

Working notes for the sampling-stack moiré work. Branch: `claude/zen-tharp-cfc5df`.
Read this top-to-bottom before resuming — the "Diagnosis" section is why the
later pieces exist, and "State" tells you exactly what's committed vs not.

## The problem

At distance the calorimeter staves shimmer with strong moiré. Investigation
(see Diagnosis) split it into two independent axes:

1. **Shading aliasing** — the cycling absorber/scintillator/readout layer
   colors (esp. thin bright layers) are a high-frequency *material* signal that
   aliases; plus specular/normal highlight shimmer on the faceted surfaces.
2. **Geometric coverage aliasing** — the thin slab edges and the sub-slab air
   gaps: *which triangle wins each pixel*, decided before shading. This is the
   dominant residual when zoomed out.

Two axes ⇒ two families of fix. Material-space tricks (averaging, roughening)
only ever touch axis 1. Axis 2 needs supersampling or geometric LOD.

## Diagnosis (conclusions, so we don't relitigate)

- MSAA / render-scale SSAA only **relocate** the moiré ("it dances") because a
  fixed sample grid can't reach Nyquist on sub-pixel geometry. Confirmed live.
- Jittered **temporal accumulation** is the general coverage fix: averaging the
  "dance" over static frames converges to the true footprint value. Fits
  `pause_when_static`, no reprojection needed (no motion). NOT YET BUILT.
- The thin bright "white slice" layers are **albedo**, not specular — so
  albedo averaging is the right tool for that part.
- Overdraw was already removed by `drop_coincident_faces` on the interior
  faces; the remaining gaps are **not coincident** (air-separated), so
  `drop_coincident_faces` can't touch them.
- FXAA is useless here (post-process luma edges); per-object distance LOD is
  wrong granularity (a stave spans near+far) — the fix must be per-pixel
  (prefilter / accumulation) or finer-than-object geometry (hull LOD).

## ODD geometry structure (verified via `nodehammer inspect tree`)

```
ECalBarrel [subdetector]            <- the ring (CONCAVE, has beam hole)
  stave1 .. stave16                 <- each stave IS a ~22.5° azimuthal wedge
    stave_inner_0
      layer1_0 .. layerN
        slice1_0 .. slice8_7 [leaf] <- the slabs (8/layer ECal, 4/layer HCal)
```

- HCalBarrel: same shape. Endcaps: `stave_outer_0..15` (16 petal-wedges).
- **The wedge IS the stave** (~16/ring, direct children of the subdetector).
  There is NO intermediate "group of staves" node. So the config-clean hull
  granularity is the existing `stave*` rule; the only coarser node is the whole
  (concave) subdetector.
- `merge_descendants` is already tagged on the `stave*` rules
  (`fixtures/configs/odd/calorimeters.toml`).

## State

### Committed on `claude/zen-tharp-cfc5df`

- `db38405` — **Overdraw diagnostic heatmap** (`DebugView::Overdraw`). Scene
  re-renders through an additive/no-depth pipeline; composite maps the per-pixel
  fragment count through a jet ramp. View panel → debug view → "overdraw" +
  range slider.
- `8fdd8c6` — **Material-stack prefilter** (axis-1 shading fix):
  - Config flag `average_material_stack` (per-rule, gated on `merge_descendants`,
    independent of `drop_coincident_faces`). Full TOML + Lua plumbing. Tags
    merged stack meshes with a `StackAverage` (avg color + feature size).
  - Scene shader blends albedo toward the average once the pixel footprint
    outgrows the feature size; plus specular AA (roughen with the same footprint
    factor + geometric specular AA from normal variance).
  - Runtime toggle `enable_material_prefilter` + `material_prefilter_scale`
    slider. Enabled on ECal/HCal staves in the ODD config (tracker excluded).

### Uncommitted (working tree) — the hull-LOD prototype + a fix

9 modified files (all part of ONE logical change, commit them together):
`render.hpp`, `boolean_tessellator.{hpp,cpp}`, `tessellation_pass.cpp`,
`render_quality.hpp`, `scene_renderer.{hpp,cpp}`, `app.cpp`,
`view_panel.cpp`.

What it does:

- **`convexHull(points)`** helper in `boolean_tessellator.cpp` — manifold
  `Hull()` + `manifoldToMesh` → flat-shaded watertight proxy. (Manifold has
  ONLY convex hull; concave shapes must be recovered with `Hull() - solid`
  booleans, e.g. `Hull(allStaves) - beamCylinder` for the subdetector shell.)
- At merge time, for each `average_material_stack` stack, also build the convex
  hull of the whole stack, paint it the stack average (matte, metallic 0,
  roughness 1), and attach it as `RenderNode::lodProxyBindings`. Instanced per
  prototype via the merge cache (`MergeResult{detail, proxy}`).
- Renderer: `DrawGroup::LodRole {Normal, Detail, Proxy}`; a stack emits a Detail
  group (slabs) + a Proxy group (hull). The cull loop draws exactly one based on
  `RenderFlags::hull_lod`. Runtime toggle `lod_hull_preview` ("hull LOD
  (preview)" in the View panel) — GLOBAL swap for now, not per-distance.
- **Cut-invariant, shape-agnostic average**: `C_avg` is **volume-weighted**
  (divergence theorem over each slab mesh), NOT area- or thickness-weighted.
  - Area-weighting was cut-sensitive (a phi wedge cut changes the tessellated
    faces → the cut scene's hull got a different color than the base scene).
  - Thickness-weighting (min-bbox-dim) fixed the cut issue but assumed thin flat
    axis-aligned plates.
  - Volume assumes nothing about shape, is invariant to a proportional phi cut
    (same volume fraction removed from every slab → ratios preserved), and
    matches thickness/area weighting for uniform thin plates. This is the
    current choice.
  - Feature size still uses the min-bbox-dim thin-plate heuristic, but it only
    sets the prefilter transition distance (tunable via the scale slider), not
    the color — much less sensitive.

Verified headlessly: compiles, all `[tessellation]`/`[ir]` tests pass, ODD
converts clean — `Hull()` runs on all ~96 calo stack prototypes, +1632 tris
total (~17/hull), meshes 1063→1159, materials 14→110.

NOT yet verified in the running viewer (the exe relink kept getting blocked by
the running viewer — LNK1168 "cannot open nodehammer.exe for writing"; a
non-issue on a fresh checkout).

## Next steps (in order)

1. **Relink + eyeball the hull LOD** in the viewer: View panel → "hull LOD
   (preview)". Check the hulled far-field reads as approximately the calo, and
   that the **cut vs uncut hull color is now consistent** (the volume-weighting
   fix). If the inner-arc bulge of a wedge reads wrong, add the
   `Hull() - beamCylinder` carve.
2. **Commit the hull-LOD prototype** (the 9 uncommitted files) once it looks
   right. Suggested message scope: hull LOD proxy generation + LOD-role draw
   gating + volume-weighted cut-invariant average.
3. **Per-distance LOD selection**: replace the global `hull_lod` toggle with
   per-instance selection by projected screen size (detail up close, hull far).
   The render IR (`lodProxyBindings`) and role-gated draw path already support
   two representations; this is a per-instance choose-and-draw in the renderer.
4. **Subdetector shell** (far LOD, concave): tessellate the mother volume solid
   (currently `skip_geometry` on `ECalBarrel` etc.), or `Hull(allStaves) -
   beamCylinder`, painted `C_avg`. Handles extreme zoom where ~16 wedges/ring
   still leaves inter-wedge gaps.
5. **Temporal accumulation** (the axis-2 general fix, still unbuilt): jittered
   projection + running average of static frames, reset on any change (camera,
   resize, render-scale step, config, quality, build). Slots into the
   `pause_when_static` settle window. Fixes coverage everywhere (tracker, gaps,
   edges) regardless of LOD. This is the higher-ROI general fix and complements
   LOD.
6. Optional: a dedicated `lod_hull` config flag if we want the hull independent
   of the shading prefilter (currently the hull reuses the
   `average_material_stack` tag as its "this is a stack" marker). The
   `average_material_stack` plumbing is the template — see the 9 layers it
   touches (config_ast, config_keys, config_loader ×2, lua_config,
   config_writer ×2, ResolvedTessellation + resolve + MergeCacheKey + hash).

## Build / test / try

Windows build goes through a VS dev env (the `just` recipes shell to
`scripts/windows-msvc-cmake.ps1`, but `just`'s OS detection can pick the wrong
branch here). The reliable incantation that worked this session:

```
# from the repo root, with build/RelWithDebInfo already conan-configured:
"<VsDevCmd.bat>" -arch=x64 -host_arch=x64 && cd /d <repo> \
  && call build\RelWithDebInfo\generators\conanbuild.bat \
  && cmake --build --preset conan-relwithdebinfo --target nodehammer nodehammer_tests
```

- First-time worktree setup: `conan install . -s build_type=RelWithDebInfo
  --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja -o '&:viewer=True'`
  then `cmake --preset conan-relwithdebinfo --fresh -GNinja -DNODEHAMMER_WITH_VIEWER=ON`.
- Run tests: `build/RelWithDebInfo/tests/nodehammer_tests.exe "[tessellation]"`.
- Convert ODD end-to-end (exercises the merge/hull on real calo):
  `build/RelWithDebInfo/nodehammer convert -i odd.nhb.zst -c fixtures/configs/odd.toml -o build/odd.glb`.
- Viewer: `build/RelWithDebInfo/nodehammer viewer` (loads `odd.nhb.zst`).
  Toggles live under View panel → Render Quality: "stack prefilter (AA)" +
  "prefilter scale", "hull LOD (preview)", debug view → "overdraw".
- Known gotcha: if the viewer is running, `nodehammer.exe` can't relink
  (LNK1168). Close it first.

## Key files

- `src/tessellation/tessellation_pass.cpp` — `tessellateMergeDescendants`:
  `StackAverage` (volume-weighted C_avg + percentile feature size), hull proxy
  gen, `MergeResult` cache.
- `src/tessellation/boolean_tessellator.{hpp,cpp}` — `convexHull`.
- `src/ir/render.hpp` — `StackAverage`,
  `RenderNode::lodProxyBindings`.
- `shaders/scene.glsl` — prefilter blend + specular AA (`stack_prefilter`,
  `mode_flags.w` enable).
- `src/viewer/scene_renderer.{hpp,cpp}` — `GpuMesh` stack fields,
  `DrawGroup::LodRole`, LOD-gated cull/draw, `RenderFlags::{material_prefilter,
  material_prefilter_scale, hull_lod}`.
- `src/viewer/render_quality.hpp` — the runtime toggles/sliders.
- `src/viewer/ui/view_panel.cpp` — the UI controls.
- `fixtures/configs/odd/calorimeters.toml` — `average_material_stack = true` on
  the calo stave rule.
- config plumbing for `average_material_stack`: `config_ast.hpp`,
  `config_keys.hpp`, `config_loader.cpp`, `lua_config.cpp`, `config_writer.cpp`.

## Open questions / caveats

- Hull convexity: a stave wedge's inner arc is chorded straight by the convex
  hull (bulges toward the beam). Likely invisible at LOD distance / occluded;
  `Hull() - beamCylinder` is the fix if not.
- Inter-wedge gaps remain at the per-stave hull level; only the subdetector
  shell closes those (step 4).
- Whether hull LOD or temporal accumulation should land first: temporal is the
  general coverage fix and helps at all zooms; hull LOD also buys back the
  D3D11 geometry-density perf ceiling at zoom-out. Not mutually exclusive.
