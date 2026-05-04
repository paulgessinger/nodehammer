#pragma once

#include <sokol_gfx.h>

namespace nodehammer::viewer {

/// Whether the active sokol_gfx backend should use reversed-Z.
///
/// Reversed-Z (near→1, far→0, GREATER_EQUAL, depth clear 0.0) gives ~6
/// orders of magnitude of usable depth precision on backends with `[0,1]`
/// clip-space depth (Metal, D3D11/12, WebGPU, Vulkan). On those backends
/// the precision benefit is automatic.
///
/// **GLES3 is the exception.** GLES3 has `[-1,1]` clip-space depth and
/// — unlike desktop GL 4.5 — does not expose `glClipControl`, so there's
/// no way to switch its NDC depth range to `[0,1]`. Reversed-Z math
/// silently degrades to normal-Z precision (z-fighting on close surfaces)
/// AND defeats the driver's standard Hi-Z heuristics (overdraw isn't
/// rejected early, fragment-bound regressions when zoomed in). On that
/// one backend we fall back to normal-Z (LESS_EQUAL, depth clear 1.0,
/// near→0, far→1).
///
/// All four convention sites — projection matrix, pipeline `depth.compare`,
/// pass action `depth.clear_value`, and the composite shader's depth
/// linearization — must agree. This helper is the single source of truth.
inline bool useReversedZ() { return sg_query_backend() != SG_BACKEND_GLES3; }

/// Whether the active backend should write a logarithmic depth value in the
/// vertex shader (overriding the standard perspective-divide depth).
///
/// Reversed-Z + 32F gives ~30+ effective bits *near the camera*, which is
/// the precision-optimal choice for typical scenes. On GLES3 we fall back
/// to normal-Z (see `useReversedZ`) — but normal-Z + 32F gives only
/// ~24 effective bits roughly uniformly across `[0,1]`, which is not
/// enough for legitimately close detector surfaces away from the near
/// plane. Logarithmic depth (`gl_Position.z = log2(1+w) * fc - 1`) gives
/// ~32-bit-equivalent precision distributed near-uniformly across the
/// entire near→far range, regardless of clip-space depth convention.
///
/// Cost: one log per vertex; mild interpolation artifact for very large
/// triangles (depth interpolated linearly across the triangle while the
/// function is non-linear). For nodehammer's small-triangle detector
/// geometry this is invisible.
///
/// Mutually exclusive with reversed-Z: when log depth is on, the depth
/// convention underneath is normal-Z (depth.compare = LESS_EQUAL,
/// depth-clear = 1.0) because that's what GLES3 already uses.
inline bool useLogDepth() { return sg_query_backend() == SG_BACKEND_GLES3; }

/// HDR color format for the offscreen scene target, or `SG_PIXELFORMAT_NONE`
/// if the backend can't render+blend RGBA16F.
///
/// WebGPU mandates RGBA16F as renderable+blendable, so this always returns
/// RGBA16F there. WebGL2 needs `EXT_color_buffer_half_float` (or the
/// stronger `EXT_color_buffer_float`, which implies it); sokol probes both
/// at init and surfaces the result through `sg_query_pixelformat`. On a
/// constrained context that lacks both extensions, this returns
/// `SG_PIXELFORMAT_NONE` and the renderer falls back to the swapchain's
/// LDR format.
inline sg_pixel_format pickHdrColorFormat() {
    sg_pixelformat_info info = sg_query_pixelformat(SG_PIXELFORMAT_RGBA16F);
    return (info.render && info.blend) ? SG_PIXELFORMAT_RGBA16F : SG_PIXELFORMAT_NONE;
}

inline bool hdrSupported() { return pickHdrColorFormat() != SG_PIXELFORMAT_NONE; }

} // namespace nodehammer::viewer
