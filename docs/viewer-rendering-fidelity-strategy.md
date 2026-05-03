# Viewer rendering fidelity strategy

Plan for improving the viewer's visual quality while keeping rendering
features additive, configurable, and safe for geometry with very close
surfaces.

This document is forward-looking. It describes a staged target shape for the
viewer renderer; it is not an implementation record.

## Status

Steps 1–5 of section 12 are landed. The renderer now runs through an
offscreen color+depth target with a fullscreen composite pass into the
swapchain; ImGui still renders in the swapchain pass. Reversed-Z is
preserved end-to-end. A `RenderQualitySettings` struct + UI panel are in
place — most fields are no-ops, wired to advertise the next phases without
implementing them yet:

- `RenderQualitySettings` (`include/nodehammer/viewer/render_quality.hpp`):
  `render_scale`, `enable_hdr`, `enable_tonemap`, `enable_fxaa`,
  `msaa_samples`, `ibl_quality`, `enable_bloom`, `debug_view`.
  `debug_view` and `enable_fxaa` are live today.
- `SceneRenderTarget` (`src/viewer/scene_render_target.{hpp,cpp}`): owns
  the offscreen color (RGBA8) + depth (`SG_PIXELFORMAT_DEPTH`) images,
  attachment views for the scene pass, texture views + sampler for the
  composite pass. Lazily (re)allocated by `App::Impl::ensureSceneTarget`
  on framebuffer-size change.
- `CompositePass` (`src/viewer/composite_pass.{hpp,cpp}`) +
  `shaders/composite.glsl`: fullscreen-triangle VS (no vbuf), FS with
  three modes — color passthrough, raw depth, linearized reversed-Z.
  Composite pipeline has no depth, no cull. `App::Impl::render` is now
  scene-pass-into-offscreen → swapchain-pass-with-composite-then-ImGui.
  Depth-store is enabled on the scene pass so the debug view can sample
  it. The color-passthrough branch now also runs an FXAA 3.11
  console-quality variant (green-channel luma, five-tap edge detect +
  two-tap final blend) gated by `quality.enable_fxaa`; depth debug views
  always short-circuit FXAA.
- "Render Quality" section in the View panel
  (`src/viewer/ui/view_panel.cpp`): live debug-view combo
  (off / depth / linear depth) and live FXAA toggle (greyed out while a
  depth debug view is active); HDR/tonemap/MSAA/bloom/render-scale/IBL
  controls present but disabled, tooltipped "wired, not implemented".

What this unlocks: HDR, tonemap, MSAA, render scale, bloom, TAA can now
land without restructuring the pass topology — they hang off the
composite shader / target format / sample count. The composite UBO has a
reserved `vec4` slot, so HDR/tonemap (step 6) can slot into the same FS
without further uniform-block surgery.

Remaining (steps 6–11): HDR + tonemap, expanded GPU material (emissive,
alpha mask), IBL quality presets, optional MSAA, adaptive quality, then
bloom / TAA / shadows / richer PBR.

---

## 1. Current rendering shape

The viewer currently renders the scene directly into the swapchain pass, then
submits Dear ImGui into the same pass and commits. The scene renderer is a
forward renderer built on sokol_gfx:

- one generated scene shader (`shaders/scene.glsl`)
- two scene pipelines, differing by cull mode
- per-mesh immutable vertex/index buffers
- per-group instanced draws keyed by `(mesh, material)`
- CPU frustum culling plus optional angle-cut culling
- reversed-Z depth testing (`GREATER_EQUAL`, depth clear = `0.0`)
- optional shader branch for metallic-roughness PBR with procedural IBL

The PBR path already exists, but the current pipeline has no offscreen render
target, HDR/tonemap pass, post-processing stack, explicit anti-aliasing pass,
or adaptive quality controller.

---

## 2. Non-negotiable depth requirements

The renderer must treat depth precision and z-fighting as a primary design
constraint. Nodehammer scenes can contain detector surfaces that are extremely
close in depth, sometimes intentionally. Visual upgrades must not accidentally
make those cases worse.

### 2.1 Preserve reversed-Z

The current reversed-Z setup should remain the baseline:

- projection maps near depth to larger values and far depth to smaller values
- depth buffer clears to `0.0`
- depth compare is `GREATER_EQUAL`
- depth writes stay enabled for opaque geometry

Any offscreen depth target, MSAA depth target, shadow pass, picking pass, or
depth-aware post-process must use the same convention unless there is a very
specific reason not to. Mixing normal-Z and reversed-Z inside the renderer is
too easy to get wrong.

#### Backend caveat: GLES3 falls back to normal-Z

Reversed-Z's precision benefit relies on a `[0, 1]` clip-space depth range —
which Metal, D3D11/12, WebGPU, and Vulkan provide natively. Desktop OpenGL
defaults to `[-1, 1]` but exposes `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`
(GL 4.5 / `ARB_clip_control`) to switch. **GLES3 does not expose
`glClipControl` at all.** On GLES3 reversed-Z math silently degrades to
normal-Z precision, and several drivers also disable Hi-Z heuristics under
reversed-Z — producing both z-fighting on close surfaces and a fragment-bound
perf regression that scales with on-screen triangle area.

The viewer therefore branches the depth convention by backend at runtime via
`useReversedZ()` (`include/nodehammer/viewer/backend_caps.hpp`):

- on `[0, 1]` clip-depth backends: reversed-Z, `GREATER_EQUAL`, depth-clear
  `0.0`, projection maps near→1
- on GLES3: normal-Z, `LESS_EQUAL`, depth-clear `1.0`, projection maps near→0

This is "one convention per backend at startup", not "convention mixed inside
a frame". The four convention sites — `Camera::proj()`, the scene pipeline's
`depth.compare`, the scene pass action's `depth.clear_value`, and the
composite shader's `linearize_depth()` — all read the same flag and stay
in lockstep.

#### GLES3 also gets logarithmic depth in the VS

The convention switch alone leaves GLES3 with normal-Z + 32F, which gives
~24 effective bits roughly uniformly across `[0, 1]`. That's enough for
most scenes but **not** for legitimately close detector surfaces sitting
away from the near plane: float density is concentrated near 0.0, so
normal-Z's "uniform" distribution actually leaves the far half of the
range with poor precision. Reversed-Z would put the precision in the
right place, but isn't available on GLES3.

The fallback is logarithmic depth written from the vertex shader:
`gl_Position.z = (log2(1 + w) * (2 / log2(far + 1)) - 1) * w`. After the
perspective divide this gives `log2(1 + view_z) / log2(1 + far)` in the
depth buffer — near-uniform precision across the entire range,
independent of the clip-space depth convention. Effective precision is
~32-bit-equivalent everywhere instead of ~24-bit clustered near the
camera.

Cost: one log per vertex (negligible on every modern GPU). Caveat: depth
is interpolated linearly across each triangle while the function is
non-linear, so very large triangles can show artifacts. Not a problem for
nodehammer's small-triangle detector geometry; would matter for
large-triangle world-scale terrain. Gated by `useLogDepth()` (also in
backend_caps.hpp), currently true only on GLES3.

When log depth is on, the underlying depth convention is normal-Z (the
log formula's output is monotonically increasing from near→far in `[0, 1]`).
The composite shader's `linearize_depth()` gains a third mode (2.0 = log-Z)
that inverts the formula via `pow(far + 1, d) - 1`.

### 2.2 Keep near/far planes tight

Depth precision still depends heavily on camera clip ranges. The camera should
continue deriving scene-aware near/far values, and future quality features
should avoid widening them casually.

Rules:

- never set a fixed huge far plane as part of a visual feature
- avoid making the near plane smaller than necessary
- recompute clip ranges from framed scene bounds and camera distance
- expose debug readouts for near, far, and far/near ratio
- prefer scene-scale-aware constants over world-unit magic numbers

If a feature needs extra depth range, it should document why and show the
measured effect on close-surface scenes.

### 2.3 Avoid polygon offset as a general fix

Polygon offset can hide z-fighting for overlays, outlines, and helper
geometry, but it should not become the answer for normal scene rendering.

Acceptable uses:

- optional wireframe overlay
- selection outline prepass
- helper grids or axes
- decals or labels that are explicitly non-physical overlays

Avoid:

- offsetting all scene geometry
- offsetting one material class just to make a test scene look stable
- using large fixed offsets that vary across backends

When offset is used, make it a named setting with conservative defaults and a
debug display.

### 2.4 Treat MSAA carefully

MSAA improves geometric edge aliasing, but it does not solve z-fighting. It can
also make depth behavior harder to inspect because coverage changes per sample.

MSAA should be optional and implemented only after the renderer has a clean
offscreen target abstraction. It must:

- use a depth buffer with matching sample count
- resolve color only, not depth, unless a later pass explicitly needs resolved
  depth
- keep the same reversed-Z compare and clear values
- be easy to disable when diagnosing depth artifacts

The first anti-aliasing step should likely be FXAA, not MSAA, because FXAA is
post-process-only and does not perturb depth behavior.

### 2.5 Be conservative with depth-aware post effects

Features such as SSAO, outlines, depth of field, contact shadows, and
screen-space reflections depend on depth reconstruction. They must understand
the renderer's reversed-Z projection and backend clip-space differences.

Before adding any depth-aware effect, add a small debug view that visualizes
linearized depth. This gives us a way to verify the math before chasing visual
artifacts in the effect itself.

---

## 3. Foundational architecture: offscreen render stack

The first structural upgrade should be an offscreen scene target:

```cpp
struct SceneRenderTarget {
    sg_image color;
    sg_view color_view;
    sg_image depth;
    sg_view depth_view;
    uint32_t width;
    uint32_t height;
    int sample_count;
    sg_pixel_format color_format;
};
```

Target responsibilities:

- allocate color/depth images at framebuffer size times render scale
- recreate resources on resize, DPI change, render scale change, MSAA change,
  or HDR format change
- begin a scene pass with reversed-Z depth clear
- expose color output to a fullscreen composite pass
- keep ImGui rendering in the final swapchain pass

Initial pass order:

1. Scene pass into offscreen color/depth.
2. Fullscreen composite pass into swapchain.
3. ImGui pass into swapchain.

This keeps the current renderer recognizable while unlocking HDR,
tonemapping, FXAA, bloom, render scale, and future TAA.

---

## 4. HDR, tonemapping, and output color

The current PBR shader writes lighting directly to the swapchain. A better PBR
path needs a higher-range intermediate color target and a controlled output
transform.

Target behavior:

- render scene lighting into HDR where supported
- fall back to LDR on limited backends
- run a fullscreen tonemap/composite shader
- expose exposure and tonemap operator settings
- keep a simple output path for compatibility and debugging

Candidate tonemappers:

- ACES fitted curve
- AgX-style curve
- simple Reinhard for debug/comparison

The composite shader is also the natural place for FXAA and optional dithering.

---

## 5. PBR material upgrades

The render IR already carries more material information than the viewer uploads
to the GPU. The current GPU material effectively uses:

- base color
- metallic factor
- roughness factor

Additive material upgrades:

- emissive factor
- alpha mask
- alpha blend, once sorting rules are clear
- specular factor and specular color
- IOR-adjusted Fresnel base reflectance
- clearcoat factor and clearcoat roughness
- anisotropy as a later feature, because it likely needs tangents or a stable
  generated frame
- transmission as an approximate mode first, not full physically correct glass

Implementation direction:

- expand `GpuMaterial` in small steps
- keep legacy Lambert and basic PBR modes available
- prefer compile-time shader variants or separate pipelines once branching
  becomes too expensive or too hard to reason about
- add debug material modes for albedo, roughness, metallic, normal, and
  emissive

---

## 6. IBL and environment quality

The current IBL is procedural, cached, and uploaded as RGBA8 textures for wide
backend compatibility. That is a good baseline, but it limits highlight range
and environment fidelity.

Additive upgrades:

- environment quality levels for cubemap size and sample count
- optional higher-precision IBL formats on backends that support them
- RGBA8 fallback for WebGL and constrained platforms
- multiple procedural environment presets
- visible environment background instead of only a flat clear color
- environment rotation coupled to sun direction
- user controls for ambient intensity and sun intensity

IBL upgrades should remain cacheable. Cache keys need to include all quality
and environment parameters that affect baked output.

---

## 7. Anti-aliasing roadmap

Anti-aliasing should be staged so we improve edges without destabilizing depth
behavior.

### 7.1 FXAA first

FXAA is the safest first AA feature:

- no depth dependency
- no MSAA image allocation
- works after tonemapping or inside the composite shader
- easy to toggle
- cheap enough for adaptive quality

The main downside is some softening, which is acceptable as a setting.

### 7.2 Optional MSAA second

MSAA can follow once offscreen target allocation is solid.

Settings:

- `1x`
- `2x`
- `4x`

It should be disabled automatically on backends or devices where image format
or memory constraints make it unreliable. It must be independently toggleable
from FXAA.

### 7.3 TAA later

TAA is the best long-term option for shimmering, but it has more moving parts:

- jittered projection
- history color buffer
- camera reprojection
- reset on camera cuts, project loads, resize, render-scale changes, and
  quality changes
- rejection/clamping to avoid ghosting

TAA should come after the renderer has stable HDR/composite infrastructure and
good debug views.

---

## 8. Specular and geometry aliasing controls

PBR can introduce highlight shimmer even when geometry edges are anti-aliased.
This is especially relevant for dense technical meshes and hard normals.

Potential controls:

- roughness floor
- direct specular intensity clamp
- presentation-mode material roughening
- optional normal smoothing at tessellation time for selected primitives
- future normal-variance roughness adjustment if we add richer vertex data

These should be settings, not hidden behavior, because users may need exact
material inspection.

---

## 9. Lighting upgrades

The current renderer uses one hardcoded directional light plus IBL. Better
lighting can remain simple and predictable:

- editable sun direction
- sun intensity
- ambient/IBL intensity
- fill light toggle
- lighting presets
- neutral technical inspection preset
- high-contrast presentation preset

Shadows are deliberately not first-wave work. They are expensive, backend
sensitive, and introduce their own depth precision problems. If added later,
they should be optional and implemented with a clear bias strategy and debug
visualization.

---

## 10. Adaptive quality

Visual features should be individually controllable and also available through
quality presets.

```cpp
enum class QualityPreset {
    Performance,
    Balanced,
    High,
    Ultra,
    Adaptive,
};

struct RenderQualitySettings {
    QualityPreset preset;
    bool enable_pbr;
    bool enable_hdr;
    bool enable_tonemap;
    bool enable_fxaa;
    int msaa_samples;
    float render_scale;
    int ibl_quality;
    bool enable_bloom;
};
```

Adaptive quality should use hysteresis:

- downgrade only after several seconds below the target frame rate
- upgrade only after sustained headroom
- change one quality step at a time
- avoid changing quality while the user is actively interacting unless the
  frame rate is very poor
- never silently change settings in manual preset modes

Likely adaptive order:

1. Lower render scale.
2. Disable bloom or expensive post effects.
3. Disable MSAA.
4. Lower IBL quality for future bakes.
5. Fall back from extended PBR to basic PBR.
6. Fall back to Lambert only as an explicit low-power mode.

---

## 11. Debug and validation views

Rendering changes need cheap ways to diagnose precision and quality problems.

Add debug views alongside the fidelity work:

- depth buffer visualization
- linearized depth visualization
- normal visualization
- albedo/base color
- roughness
- metallic
- emissive
- draw group / material ID false color
- overdraw or wireframe overlay
- render target format/sample count display
- near/far/far-near readout

Close-surface regression scenes should be kept around and used when changing
projection, depth target allocation, MSAA, or depth-aware effects.

---

## 12. Suggested implementation order

1. Add render-quality settings and UI plumbing, with no behavior change.
2. Introduce offscreen scene color/depth target, still LDR and no post effect.
3. Add fullscreen composite pass and keep output visually identical.
4. Add depth debug visualization and verify reversed-Z reconstruction.
5. Add FXAA in the composite pass.
6. Add HDR color target where supported plus tonemapping.
7. Expand GPU material data with emissive and alpha mask.
8. Add IBL quality presets and environment controls.
9. Add optional MSAA target allocation and resolve.
10. Add adaptive quality controller.
11. Consider bloom, TAA, shadows, and richer PBR extensions after the core
    path is stable.

The important sequencing rule: any feature that touches depth comes after the
offscreen target abstraction and depth debug view exist.
