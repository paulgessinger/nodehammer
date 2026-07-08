# Live AO: plan of record

How the viewer does ambient occlusion, and why. Written 2026-07-08 after the
baked-AO and GPU-bake experiments were both abandoned.

## Decision history (short version)

1. **Screen-space GTAO** is the live AO. It was frame-late and expensive; this
   branch fixed the lag and cut the cost (below).
2. **Baked per-instance vertex AO** was prototyped (ray-traced sidecar cache,
   face subdivision) to replace GTAO for rigid detector geometry. It looked
   right but the CPU bake was too slow for interactive use (~11 min pristine on
   ODD at N=32; ~1 s wedge-cut rebake). Preserved in branch `refactor/baked-ao`.
3. **GPU compute bake** was spiked to rescue the bake time. Measured **NO-GO**:
   ~20 M rays/s on an M4 Pro (≈2× the CPU baker), far short of the ≥300 M/s that
   would have made it fly — incoherent BVH2 traversal is a latency wall on GPUs
   without hardware ray tracing, and hardware RT isn't reachable through sokol
   and leaves the web out. Full write-up + reproducible harness in branch
   `refactor/gpu-ao` (`bake-ao --gpu` / `--dump-gpu-data`).

**Net:** baked AO is out; live GTAO is the AO path (or no AO — see WebGL2).
The baked/GPU code lives only in those two branches, not here.

## What this branch changed

### 1. Frame-lag fix — consume AO current-frame in the composite

The bug: the scene shader sampled the *previous* frame's denoised GTAO for its
PBR IBL term (bent-normal irradiance + multi-bounce + specular occlusion),
because a forward renderer can't read this frame's AO before it shades without
a depth prepass. During camera motion, occlusion visibly dragged behind the
geometry.

The fix: apply AO in the **composite pass**, which already runs *after* the AO
pass — a single current-frame scalar multiply on the lit color. Consequences:

- Removed the scene shader's advanced-AO path (bent-normal IBL, multi-bounce,
  specular occlusion) and the GTAO bent-normal *production* in `ao.glsl` /
  `ao_denoise.glsl` that fed it. AO is now a single R channel end to end.
- Collapsed the `ao_hist[2]` ping-pong to a single `ao_rt` written and consumed
  in the same frame — no frame-late read, so the ping-pong (which existed to
  protect the read buffer during a resolution-scale resize) is gone, along with
  its gating state (`ao_history_valid`, `last_scene_consumed_ao`, cut-flip
  invalidation).
- Dropped the `enable_advanced_ao` / `ao_bent_strength` settings, UI, and
  persistence.

**Tradeoff, accepted:** AO is now a flat readability multiply on final color,
losing the bent-normal fill / multi-bounce / specular-occlusion niceties. This
is the deliberate "cheap readability layer, not physically-plausible occlusion"
choice — a forward renderer's only current-frame option short of a depth
prepass, which wasn't worth a second full geometry pass on five backends.

No depth prepass was added. The AO pass already reads current-frame depth (the
scene pass writes depth first); only the *consumption* point moved.

### 2. Drop AO entirely on WebGL2 (GLES3)

New `aoSupported()` in `backend_caps.hpp` returns false on GLES3. The AO target
allocation, the AO pass, and the composite multiply all consult it, and the
view panel hides the AO controls. GLES3/WebGL2 renders with no AO — the GTAO +
denoise passes were the biggest per-frame GPU cost on that constrained path,
and with baked AO gone it simply doesn't pay for AO. Metal, D3D11, desktop GL,
and WebGPU keep live GTAO.

## Remaining / optional (not done here)

Cost knobs, if live AO's per-frame cost still matters. The current default is
`ao_quality` Ultra (8×8 = 64 taps) at full resolution with the 25-tap denoise:

- Lower defaults (Medium + `ao_resolution_scale` 0.5 + explicit bilateral
  upsample) reclaim most of the cost — AO is low-frequency. Two-line change;
  measure before/after.
- The residual noise's root cause is depth-reconstructed normals (no normal
  buffer), which is why the denoise exists. If AO needs to look better, output
  view-space normals from the forward pass (MRT, WebGL2-safe) and shrink the
  kernel — only if the flat multiply proves insufficient by eye.

## Open questions

- Is there ever appetite for a non-portable hardware-RT bake (Metal intersection
  functions / DXR)? Only thing that clears the throughput bar, but backend-
  specific, a real project, and nothing for the web. Parked.
- Should the abandoned baked-AO / GPU-spike branches be kept indefinitely or
  tagged and pruned once this lands?
