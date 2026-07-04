// Shared depth helpers, pulled in via sokol-shdc's @include directive.
//
// Any depth-aware screen-space pass (composite linear-depth view, GTAO,
// future contact shadows / SSR / bent-normal AO) needs the same view-space
// reconstruction. Centralising it here avoids drift between consumers and
// keeps the depth-convention switch in exactly one place.
//
// Keep this file portable across every slang sokol-shdc emits (glsl430,
// glsl300es, metal_macos, hlsl5, wgsl). Plain math only — no
// version-specific types or extensions.

// Linearize a sampled depth value to view-space Z. Three modes selected by
// `mode`:
//   0.0 — normal-Z   (d=0 at near, d=1 at far)
//   1.0 — reversed-Z (d=1 at near, d=0 at far)
//   2.0 — log-Z      (d = log2(1+view_z) / log2(1+far))
// The depth *texture* always contains [0,1] values regardless of the
// backend's clip-space depth range. For normal/reversed we remap d to its
// reversed-Z equivalent (1-d) so a single closed form covers both. For
// log-Z we invert the VS formula directly using the far-plane parameter.
//
// `far` is only read in log-Z mode. Pass the camera far_plane there.
float linearize_depth(float d, float n, float f, float mode, float far) {
    if (mode > 1.5) {
        // max() keeps the base provably non-negative for the HLSL/FXC
        // back-end (X3571); far ≥ near > 0 in practice, so this is a no-op.
        return pow(max(far + 1.0, 0.0), d) - 1.0;
    }
    float reversed = step(0.5, mode);
    float dr = mix(1.0 - d, d, reversed);
    return (n * f) / (n + dr * (f - n));
}
