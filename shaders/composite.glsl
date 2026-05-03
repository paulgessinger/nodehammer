// Fullscreen composite — samples the offscreen scene color (and optionally
// depth) into the swapchain. Today this is a passthrough; future HDR /
// tonemap / FXAA / dither work hangs off this shader's FS.
//
// Three modes selected by a uniform `mode`:
//   0 — color passthrough (default; visually identical to direct-to-swapchain)
//   1 — raw depth (under reversed-Z closer fragments are BRIGHTER; under
//       normal-Z they are DARKER — the convention is backend-conditional,
//       see useReversedZ in C++)
//   2 — linearized depth (uniform near→far gradient regardless of convention)
//
// VS uses gl_VertexID to synthesise a fullscreen triangle, so no vertex
// buffer or input attribute is required at draw time. Caller issues
// sg_draw(0, 3, 1).

@module composite

@vs composite_vs
out vec2 v_uv;
void main() {
    // gl_VertexIndex ∈ {0,1,2} → (0,0), (2,0), (0,2). The triangle covers
    // [-1,1]^2 in NDC; v_uv ∈ [0,2] across the triangle so the sampled
    // [0,1]^2 region tiles cleanly with no scaling math in the FS.
    // sokol-shdc uses Vulkan-flavored GLSL (gl_VertexIndex, not gl_VertexID);
    // it cross-compiles to gl_VertexID on GL backends.
    //
    // The V flip required for top-left-origin backends is applied in the
    // FS based on a uniform flag (see mode_near_far.w in composite_fs).
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs composite_fs
layout(binding=0) uniform composite_params {
    // x = mode (0=color, 1=raw depth, 2=linear depth)
    // y = near plane, z = far plane
    // w = flip_v (1.0 on top-left-origin backends like Metal/D3D/WGPU,
    //             0.0 on bottom-left-origin backends like GLCore/GLES3).
    //     Sokol's framebuffer NDC y is +up across all backends, but the
    //     texture origin convention differs — without this flag the
    //     composited image is upside-down on top-left-origin backends.
    vec4 mode_near_far;
    // x = enable_fxaa (0.0 or 1.0), y = 1/render_width, z = 1/render_height,
    // w = reserved. Appended at the end so existing offsets stay stable
    // under std140 — sokol-shdc regenerates the C struct in lockstep.
    vec4 fxaa_params;
    // x = depth mode for the linear-depth debug view:
    //     0.0 = normal-Z   (d=0 at near, d=1 at far; LESS_EQUAL, clear=1)
    //     1.0 = reversed-Z (d=1 at near, d=0 at far; GREATER_EQUAL, clear=0)
    //     2.0 = log-Z      (d = log2(1+view_z) / log2(1+far_plane); GLES3
    //                       fallback — see useLogDepth in C++)
    // y = far_plane (only read in log-Z mode for the pow inversion).
    // zw reserved. The C++ side picks the mode from useLogDepth /
    // useReversedZ; the three are mutually exclusive at any moment.
    vec4 depth_params;
};

layout(binding=0) uniform texture2D scene_color;
// WebGPU rejects the default Float/filterable sample type for a Depth32Float
// texture. Mark depth as unfilterable_float and pair with a nonfiltering
// sampler (depth visualizations don't need bilinear filtering anyway).
@image_sample_type scene_depth unfilterable_float
layout(binding=1) uniform texture2D scene_depth;
layout(binding=0) uniform sampler smp_color;
@sampler_type smp_depth nonfiltering
layout(binding=1) uniform sampler smp_depth;

in vec2 v_uv;
out vec4 frag_color;

// Linearize a sampled depth value to view-space Z. Three modes selected
// by `mode`:
//   0.0 — normal-Z   (d=0 at near, d=1 at far)
//   1.0 — reversed-Z (d=1 at near, d=0 at far)
//   2.0 — log-Z      (d = log2(1+view_z) / log2(1+far))
// The depth *texture* always contains [0,1] values regardless of the
// backend's clip-space depth range. For normal/reversed we remap d to its
// reversed-Z equivalent (1-d) so a single closed form covers both. For
// log-Z we invert the VS formula directly using the far-plane parameter.
float linearize_depth(float d, float n, float f, float mode, float far) {
    if (mode > 1.5) {
        return pow(far + 1.0, d) - 1.0;
    }
    float reversed = step(0.5, mode);
    float dr = mix(1.0 - d, d, reversed);
    return (n * f) / (n + dr * (f - n));
}

// FXAA 3.11 console-quality variant. Five luma taps (center + 4 corners) +
// edge-direction blend + two final taps. Green-channel luma is the LDR-sRGB
// approximation from NVIDIA's reference console path; revisit for proper
// rec.709 luma when HDR/tonemap lands (strategy step 6).
const float FXAA_EDGE_THRESHOLD     = 0.125;   // 1/8: skip flat regions
const float FXAA_EDGE_THRESHOLD_MIN = 0.0625;  // 1/16: skip dark noise
const float FXAA_SPAN_MAX           = 8.0;

vec3 fxaa(vec2 uv, vec2 inv_res) {
    vec3 rgb_m  = texture(sampler2D(scene_color, smp_color), uv).rgb;
    vec3 rgb_nw = textureOffset(sampler2D(scene_color, smp_color), uv, ivec2(-1, -1)).rgb;
    vec3 rgb_ne = textureOffset(sampler2D(scene_color, smp_color), uv, ivec2( 1, -1)).rgb;
    vec3 rgb_sw = textureOffset(sampler2D(scene_color, smp_color), uv, ivec2(-1,  1)).rgb;
    vec3 rgb_se = textureOffset(sampler2D(scene_color, smp_color), uv, ivec2( 1,  1)).rgb;

    float luma_m  = rgb_m.g;
    float luma_nw = rgb_nw.g;
    float luma_ne = rgb_ne.g;
    float luma_sw = rgb_sw.g;
    float luma_se = rgb_se.g;

    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
    float range    = luma_max - luma_min;

    if (range < max(FXAA_EDGE_THRESHOLD_MIN, luma_max * FXAA_EDGE_THRESHOLD)) {
        return rgb_m;
    }

    vec2 dir;
    dir.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
    dir.y =  ((luma_nw + luma_sw) - (luma_ne + luma_se));

    float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * 0.5, 1.0/128.0);
    float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * inv_res;

    vec3 rgb_a = 0.5 * (
        texture(sampler2D(scene_color, smp_color), uv + dir * (1.0/3.0 - 0.5)).rgb +
        texture(sampler2D(scene_color, smp_color), uv + dir * (2.0/3.0 - 0.5)).rgb);
    vec3 rgb_b = rgb_a * 0.5 + 0.25 * (
        texture(sampler2D(scene_color, smp_color), uv + dir * -0.5).rgb +
        texture(sampler2D(scene_color, smp_color), uv + dir *  0.5).rgb);

    // If the second-pass luma escapes the corner range, the edge direction
    // was too oblique — fall back to the first-pass blend.
    float luma_b = rgb_b.g;
    return (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
}

void main() {
    int mode = int(mode_near_far.x);
    float flip_v = mode_near_far.w;
    vec2 uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, flip_v));
    if (mode == 1) {
        float d = texture(sampler2D(scene_depth, smp_depth), uv).r;
        frag_color = vec4(vec3(d), 1.0);
    } else if (mode == 2) {
        float n = mode_near_far.y;
        float f = mode_near_far.z;
        float d = texture(sampler2D(scene_depth, smp_depth), uv).r;
        float zv = linearize_depth(d, n, f, depth_params.x, depth_params.y);
        float t = clamp((zv - n) / (f - n), 0.0, 1.0);
        frag_color = vec4(vec3(t), 1.0);
    } else if (fxaa_params.x > 0.5) {
        frag_color = vec4(fxaa(uv, fxaa_params.yz), 1.0);
    } else {
        frag_color = texture(sampler2D(scene_color, smp_color), uv);
    }
}
@end

@program composite composite_vs composite_fs
