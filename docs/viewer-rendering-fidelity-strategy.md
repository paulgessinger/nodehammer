# Viewer rendering fidelity strategy

Plan for improving the viewer's visual quality while keeping rendering
features additive, configurable, and safe for geometry with very close
surfaces.

This document is forward-looking. It describes a staged target shape for the
viewer renderer; it is not an implementation record.

## Status

Steps 1–6 of the implementation order are landed: offscreen
color+depth, composite pass, depth debug, FXAA, and HDR + tonemap
(scene target promoted to RGBA16F where supported, IBL bake also
RGBA16F so the sun disc and bright sky can encode values >> 1.0).
Reversed-Z is preserved end-to-end; ImGui still renders in the
swapchain pass. A `RenderQualitySettings` struct + UI panel are in
place — most fields are no-ops, wired to advertise the next phases
without implementing them yet:

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

Remaining (steps 7+): expanded GPU material (emissive, alpha mask),
IBL quality presets, optional MSAA, adaptive quality, then
bloom / TAA / shadows / richer PBR. HDR + tonemap on its own gave a
smaller perceived-quality bump than expected against an offline
reference render — see section 12 for the gap analysis that should
shape the next steps.

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

The current IBL is procedural and cached, baked into RGBA16F cubemaps
on backends that can render+blend that format and RGBA8 elsewhere.
Range is no longer the bottleneck; **content** is. The procedural
environment is a three-stop vertical gradient (zenith / horizon /
ground) plus a soft sun spot, so reflective surfaces show no
recognizable environment and specular highlights are diffuse and
indistinct. Section 12 covers this gap in more detail; the upgrades
below are the renderer-side enablers.

Environment-content upgrades:

- **Nishita-style procedural sky** — physically motivated atmospheric
  scattering parameterized by sun direction, turbidity, ground albedo.
  Gives gradient + a real sun disc + horizon warming for free, all
  high-frequency enough to drive recognizable specular reflections.
  Bake into the existing IBL cubemap pipeline; cache key extends with
  the new parameters.
- **User-supplied HDRI** — load equirectangular `.hdr` / `.exr`
  environments and project to cubemap once at load time. This is what
  most offline references use; expose a small built-in set (neutral
  studio, dramatic outdoor, technical greybox) plus a file picker.
- **Visible background dome** — sample the environment cubemap as a
  fullscreen background in the composite pass instead of a flat clear
  color. Optional decoupling between *visible* background (e.g.
  neutral gradient) and *lighting* environment (e.g. real HDRI), the
  same split look-dev viewports use.
- **Environment rotation coupled to sun direction.**
- **User controls for ambient intensity and sun intensity.**
- Additional procedural environment presets for low-end / no-bake
  paths.

Renderer-side polish:

- environment quality levels for cubemap size and sample count
- multi-scattering compensation in the BRDF (Fdez-Agüera 2019) so
  the brighter HDR environment doesn't lose energy at high roughness

IBL upgrades should remain cacheable. Cache keys need to include all
quality and environment parameters that affect baked output (sky
model, sun direction, turbidity, source HDRI hash, sample counts).

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

### 9.1 Analytical sun vs. sky-baked sun

Once a Nishita-style procedural sky lands (section 6), it is tempting
to drop the analytical directional light and rely on the sky's own sun
disc. Don't. The disc baked into the cubemap has a fixed solid angle
and is filtered into the prefilter mips, so it can't drive the kind of
crisp specular highlights that sell metallic surfaces. Keep the
analytical sun, point its direction at the Nishita sun position, and
let the IBL provide the rest of the hemisphere. UI should expose one
"sun direction" control that drives both.

### 9.2 Shadows

Shadows are deliberately not first-wave work. They are expensive,
backend sensitive, and introduce their own depth precision problems.
A reference comparison (section 12) suggests that for typical interior
detector views a directional shadow map adds less than expected — most
of the perceived 3D in cluttered geometry comes from short-range
occlusion, which screen-space AO covers more cheaply. When shadows do
land, prefer this order:

1. **Screen-space contact shadows** — a few ray-marched samples in
   depth along the light vector. Handles the short-range case nearly
   for free, no extra render target.
2. **Single-cascade directional shadow map** — for outdoor / silhouetted
   framing where the sun direction matters globally.
3. **CSM** — only once we have outdoor or large-scale presentation
   needs.

All three must keep reversed-Z conventions and need a clear bias
strategy plus debug visualization.

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

## 12. Findings from offline reference comparison

A side-by-side comparison of the current viewer against an offline
Cycles render of the same detector exposes which gaps actually
dominate the perceived-quality delta. HDR + tonemap on its own gave a
smaller bump than expected; the items below are ranked by perceived
contribution and biased toward techniques that are realistic in real
time. They should drive the next-step ordering more than the section
12 list did.

### 12.1 Contact occlusion is the single biggest gap

The reference has dark, soft AO in every crevice between modules,
brackets, and structural plates. The current ambient is one hemisphere
lookup with no local occlusion, so concave detail looks flat and
uniformly lit even with HDR + tonemap.

Add screen-space ambient occlusion (GTAO is the modern default)
sampled in the composite pass from the offscreen depth target — the
plumbing for this already exists since steps 1–4. A normal target is
not strictly required for the first version (depth-only AO is
serviceable) but unlocks bent-normal AO later, which can feed back
into the IBL diffuse lookup as a cheap first-bounce GI proxy.

Cost is moderate: one extra pass at ~half resolution, blurred and
upsampled. Pairs naturally with the existing depth debug view.

### 12.2 Environment fidelity

The current procedural environment is a three-stop vertical gradient
(zenith / horizon / ground) with a soft sun spot. It is low-frequency
by construction, so reflective surfaces show no recognizable
environment and metallic specular reads as featureless brightness.

Two complementary upgrades, in order:

- **Nishita-style procedural sky.** Physically motivated atmospheric
  scattering with sun direction, turbidity, and ground albedo. Adds
  real horizon warming, a real sun disc, and high-frequency content
  that drives recognizable specular reflections. Bake into the
  existing IBL pipeline, extend the cache key.
- **User-supplied HDRI.** Load equirectangular `.hdr` / `.exr`
  environments. This is what offline references actually use; even a
  small built-in set (neutral studio, dramatic outdoor, technical
  greybox) transforms look-dev. RGBA8 cubemap stays the fallback for
  WebGL2 without `EXT_color_buffer_half_float`; the existing RGBA16F
  bake path covers everything else.

See section 9.1 — the procedural sun disc is **not** a substitute for
the analytical directional light; both should run together.

### 12.3 Visible background dome

The reference uses a graduated dark background; ours is a flat clear
color, which makes the scene feel "cut out". Once a real cubemap
exists (12.2), sampling it as a fullscreen background in the composite
pass is essentially free and immediately makes scenes feel framed.

Optional: decouple the *visible* background from the *lighting*
environment so a user can light with a real HDRI but display a neutral
gradient on screen — this is the standard look-dev pattern.

### 12.4 Bounce-light approximation

Cycles' interior glow comes from indirect diffuse: copper modules
brightening adjacent black backplanes, the beam pipe picking up warm
fill. We won't path-trace it, but two cheap proxies recover a lot:

- **Multi-scattering compensation in the BRDF.** Most real-time PBR
  loses energy at high roughness; the Fdez-Agüera 2019 single-pass
  term is a few lines and noticeably brightens rough metals once the
  IBL has real range.
- **Bent-normal AO into IBL diffuse.** Already implied by 12.1; uses
  the AO direction to bias the irradiance sample, approximating
  short-range first-bounce.

Injecting bright emissive surfaces into the IBL bake is also possible
but probably overkill for now.

### 12.5 Sun-driven shadows are lower priority than expected

The reference's interior 3D cues come mostly from short-range
occlusion, not from long-range cast shadow. A directional shadow map
matters when there is outdoor / silhouetted framing; for typical
interior detector views, prioritize SSAO/GTAO and screen-space contact
shadows first. See section 9.2 for the shadow ordering this implies.

### 12.6 Material differentiation — IR is richer than the GPU path

The reference clearly has distinct PBR materials per sub-detector —
glossy black backplanes, copper modules, polished metallic beam pipe.
**The IR already carries this information**; the viewer is throwing
most of it away on upload.

`RenderMaterial` (`include/nodehammer/ir/render.hpp`) carries:

- `baseColorFactor`, `metallicFactor`, `roughnessFactor` — used
- `emissiveFactor` — populated by tessellation, exported to glTF,
  **dropped by the viewer**
- `alphaMode` ("OPAQUE" / "MASK" / "BLEND") + `alphaCutoff` —
  populated, exported, **dropped**
- `doubleSided` — populated per material, **dropped**; cull mode is
  currently a global pipeline switch
- `ior`, `transmissionFactor`, `clearcoatFactor`,
  `clearcoatRoughnessFactor`, `anisotropyStrength`,
  `anisotropyRotation`, `specularFactor`, `specularColorFactor` —
  KHR-extension fields, populated, exported, **dropped**

The ODD fixture config (`fixtures/configs/odd/materials.toml`)
actively sets `ior`, `transmission`, `clearcoat`,
`clearcoat_roughness`, and `specular` for multiple materials — e.g.
the beam-pipe-liner material has `ior = 1.70`, `transmission = 0.85`,
`clearcoat = 0.1` already in the file. The Blender reference honors
these; our viewer renders them as a flat metallic-roughness
approximation.

ROI ranking of fields to wire up next, cheapest-first:

1. **`emissiveFactor`** (vec3). Trivial: add to `GpuMaterial` UBO, add
   one line in the shader after the lighting term. Lights up
   self-luminous parts immediately and is also the natural input for
   any later bloom pass.
2. **`alphaMode` + `alphaCutoff`** (MASK only). Add an `alpha_cutoff`
   to the material UBO, `discard` in the FS when `mode == MASK &&
   base_color.a < alpha_cutoff`. No sorting required, no extra pass.
   Cheap and unblocks foliage-style cutouts and any geometry the
   importer flags as masked.
3. **`doubleSided` per material.** The current global cull-mode
   toggle is too coarse — split materials into two draw groups
   keyed on `doubleSided` and pick the cull/no-cull pipeline per
   group. Removes one of the user's explicit options ("backface cull")
   in favor of correct behavior.
4. **`specularFactor` + `specularColorFactor`** (KHR_specular). Lets
   dielectrics tint and modulate their specular without the
   metallic-roughness hack. Two extra UBO floats + an `f0` adjust in
   the shader.
5. **`ior` → F0 reflectance** for non-metallic surfaces:
   `f0 = ((ior - 1) / (ior + 1))^2`. One line; pairs with the
   specular factor above and makes glass / plastic / liner materials
   read correctly.
6. **`clearcoatFactor` + `clearcoatRoughnessFactor`** (KHR_clearcoat).
   Adds a second specular lobe — significant visual cost on glossy
   plastics and lacquered surfaces (the ODD silicon module material
   already requests `clearcoat = 0.20`). Shader cost is one extra
   GGX evaluation; UBO cost is two floats.
7. **`alphaMode = BLEND`**. Lower priority because correct order-
   independent transparency means either back-to-front sort per
   frame or an OIT pass; either is real work. MASK gets us 80% of
   the visual benefit for a fraction of the cost.
8. **`transmissionFactor` (KHR_transmission)**. Real implementation
   needs a separately-rendered opaque background color (the
   "transmission framebuffer") sampled with roughness-driven mip
   selection. Significant — but the ODD beam-pipe-liner asks for
   `transmission = 0.85` so it does have a target use case. Land
   after the composite pass owns an opaque-color resolve.
9. **`anisotropyStrength` / `anisotropyRotation`**. Low priority —
   needs a stable tangent frame from generated geometry, and
   nodehammer assets don't currently produce one.

Items 1–6 are all small, additive, and don't touch sorting or extra
passes; they are the "expand `GpuMaterial` in small steps" plan from
section 5 made concrete. Doing 1–6 unlocks most of the perceived
material differentiation in the reference render without needing
order-independent transparency or transmission infrastructure.

### 12.7 Bloom and tonemap operator choice

The reference has subtle bloom on bright copper edges and a filmic
roll-off. Bloom is already on the roadmap but deserves promotion in
the order — a single Kawase blur adds disproportionate perceived
quality, especially after 12.2 introduces a real specular environment.
Tonemap operator (AgX vs ACES vs Reinhard) produces visibly different
output even at neutral exposure; expose it in the UI rather than
hardcoding one.

### 12.8 Specular and edge stability under sharper lighting

Detectors are stacks of thin parallel sensor planes — a worst case
for both geometric and specular aliasing, visible in the current
viewer as moiré across endcaps. FXAA helps the silhouettes but does
nothing for in-surface highlight shimmer, and a sharper specular
environment (12.2) will *amplify* that shimmer. The section 8 controls
(roughness floor, specular intensity clamp) become important once the
environment carries real high-frequency content.

---

## 13. Suggested implementation order

Updated to reflect the section 12 findings: prioritize the items that
actually close the gap to an offline reference — contact occlusion,
environment content, and the visible-background dome — before broader
material and AA work.

Done:

1. Render-quality settings and UI plumbing, no behavior change.
2. Offscreen scene color/depth target.
3. Fullscreen composite pass.
4. Depth debug visualization with reversed-Z reconstruction.
5. FXAA in the composite pass.
6. HDR color target plus tonemapping.

Next, ordered by perceived-quality ROI. The first three are
near-zero-cost data wiring — all the values are already in
`RenderMaterial`, just dropped on upload — so they should land
*before* the bigger environment / AO work even though each individual
item is smaller.

7. **Wire `emissiveFactor` through `GpuMaterial`.** One UBO field,
   one shader add. Self-luminous parts immediately pop and this
   becomes the input for any later bloom pass.
8. **Wire `alphaMode = MASK` + `alphaCutoff`.** One UBO field, one
   `discard` in the FS. No sorting needed.
9. **Per-material `doubleSided`.** Split draw groups by cull mode and
   pick the right pipeline per group; retire the global cull toggle.
10. **Screen-space AO (GTAO or SSAO) in the composite pass.** Highest
    single-feature impact for cluttered detector geometry; reuses the
    existing offscreen depth.
11. **Nishita procedural sky baked into the existing IBL pipeline,**
    plus environment rotation coupled to sun direction.
12. **Visible background dome** sampled from the IBL cubemap in the
    composite pass.
13. **HDRI loading** (`.hdr` / `.exr` equirectangular) with a small
    built-in set; reuse the same bake path. Adds tonemap operator
    selector while we're in the composite shader.
14. **`specularFactor` + `specularColorFactor` + `ior`-driven F0.**
    Two UBO floats, one F0 line. Makes dielectrics actually look like
    dielectrics; the ODD config already sets these on most materials.
15. **`clearcoatFactor` + `clearcoatRoughnessFactor`.** Second
    specular lobe. The ODD silicon-module material already requests
    `clearcoat = 0.20`.
16. **Bloom** in the composite pass.
17. **BRDF multi-scattering compensation** + bent-normal AO feeding
    IBL diffuse, once both AO and the high-range environment are in.
18. Optional MSAA target allocation and resolve.
19. Adaptive quality controller.
20. Screen-space contact shadows; then optional single-cascade
    directional shadow map.
21. **`alphaMode = BLEND`** (needs sorting or OIT) and
    **`transmissionFactor`** (needs an opaque-color resolve target);
    land after the composite pass infrastructure can host them.
22. TAA and **anisotropy** (needs a stable tangent frame from
    generated geometry) once the core path is stable.

The important sequencing rule still holds: any feature that touches
depth comes after the offscreen target abstraction and depth debug
view exist. The new corollary: any feature that adds high-frequency
specular (real environments, sharper lighting) should land alongside
the section 8 specular-stability controls, or it will trade one form
of "underwhelming" for one form of "shimmery".
