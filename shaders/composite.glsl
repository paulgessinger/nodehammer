// Fullscreen composite — samples the offscreen scene color (and optionally
// depth) into the swapchain. Applies exposure + tonemap (HDR → LDR) and
// optional FXAA in the same FS.
//
// Three modes selected by a uniform `mode`:
//   0 — color (default; routes through sampleScene for exposure + tonemap,
//       and optionally FXAA on the tonemapped result)
//   1 — raw depth (under reversed-Z closer fragments are BRIGHTER; under
//       normal-Z they are DARKER — the convention is backend-conditional,
//       see useReversedZ in C++)
//   2 — linearized depth (uniform near→far gradient regardless of convention)
// Depth views bypass exposure and tonemap entirely.
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
    // x = exposure (linear scalar = 2^stops, computed CPU-side).
    // y = tonemap mode (-1.0 = passthrough, 0.0 = ACES, 1.0 = Reinhard,
    //                   2.0 = AgX). The CPU side packs -1 when the user
    //                   has tonemap disabled.
    // zw reserved (bloom / dither parameters land here in later steps).
    vec4 tonemap_params;
    // x = enable_ao (0.0 or 1.0). When 0 the AO map binding is a 1×1 white
    //     dummy provided by AoPass and the multiply collapses to identity
    //     anyway, but we still gate to skip the texture fetch on the
    //     hot path. yzw reserved.
    vec4 ao_params;
};

layout(binding=0) uniform texture2D scene_color;
// WebGPU rejects the default Float/filterable sample type for a Depth32Float
// texture. Mark depth as unfilterable_float and pair with a nonfiltering
// sampler (depth visualizations don't need bilinear filtering anyway).
@image_sample_type scene_depth unfilterable_float
layout(binding=1) uniform texture2D scene_depth;
// AO is a single-channel R8 — filterable, sampled bilinearly. When AO is
// disabled the binding is a 1×1 white dummy supplied by AoPass so the
// pipeline keeps a single variant.
layout(binding=2) uniform texture2D ao_map;
layout(binding=0) uniform sampler smp_color;
@sampler_type smp_depth nonfiltering
layout(binding=1) uniform sampler smp_depth;
layout(binding=2) uniform sampler smp_ao;

in vec2 v_uv;
out vec4 frag_color;

@include depth_helpers.glsl.h

// Tonemap operators — convert linear HDR (potentially > 1.0) to LDR in
// [0,1]. ACES is the default; Reinhard is the diagnostic baseline; AgX is
// Stephen Hill's polynomial fit (drop-in replacement for ACES with a
// gentler shoulder and better hue stability).
vec3 tonemap_aces(vec3 x) {
    // Krzysztof Narkowicz fit of the ACES curve; cheap, looks good.
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 tonemap_reinhard(vec3 x) { return x / (1.0 + x); }

// AgX (Stephen Hill's fit). Input/output color matrices flank a 6th-order
// polynomial sigmoid; the matrices keep hues stable through the shoulder
// where naive curves desaturate.
vec3 agx_default_contrast_polynomial(vec3 x) {
    // Polynomial coefficients for AgX default contrast (Troy Sobotka /
    // Stephen Hill). Operates on log2-normalized input in [0,1].
    const float c0 = -0.00232;
    const float c1 =  0.07330;
    const float c2 = -0.31295;
    const float c3 =  0.53159;
    const float c4 =  0.26579;
    const float c5 =  0.43904;
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return c0 + c1 * x + c2 * x2 + c3 * x2 * x
              + c4 * x4 + c5 * x4 * x;
}

vec3 tonemap_agx(vec3 x) {
    const mat3 agx_in = mat3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 agx_out = mat3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);
    const float min_ev = -12.47393;
    const float max_ev =  4.026069;
    vec3 v = agx_in * max(x, vec3(0.0));
    v = clamp((log2(max(v, vec3(1e-10))) - min_ev) / (max_ev - min_ev), 0.0, 1.0);
    v = agx_default_contrast_polynomial(v);
    return clamp(agx_out * v, 0.0, 1.0);
}

// Sample the offscreen scene color, apply AO, exposure, then optional tonemap.
// All color paths (passthrough + FXAA) route through here so FXAA sees the
// final LDR result and its luma assumptions (rec.709) hold even when the
// scene target is RGBA16F. AO multiplies the linear scene color *before*
// exposure so the slider stays consistent with tonemap on/off.
vec3 sampleScene(vec2 uv) {
    vec3 c = texture(sampler2D(scene_color, smp_color), uv).rgb;
    if (ao_params.x > 0.5) {
        float ao = texture(sampler2D(ao_map, smp_ao), uv).r;
        c *= ao;
    }
    c *= tonemap_params.x;
    int tm = int(tonemap_params.y + 0.5);
    if      (tm == 0) c = tonemap_aces(c);
    else if (tm == 1) c = tonemap_reinhard(c);
    else if (tm == 2) c = tonemap_agx(c);
    return c;
}

float luma709(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// FXAA 3.11 console-quality variant. Five luma taps (center + 4 corners) +
// edge-direction blend + two final taps. Operates on the tonemapped LDR
// output of sampleScene with rec.709 luma — correct for both LDR and HDR
// scene targets.
const float FXAA_EDGE_THRESHOLD     = 0.125;   // 1/8: skip flat regions
const float FXAA_EDGE_THRESHOLD_MIN = 0.0625;  // 1/16: skip dark noise
const float FXAA_SPAN_MAX           = 8.0;

vec3 fxaa(vec2 uv, vec2 inv_res) {
    vec3 rgb_m  = sampleScene(uv);
    vec3 rgb_nw = sampleScene(uv + inv_res * vec2(-1.0, -1.0));
    vec3 rgb_ne = sampleScene(uv + inv_res * vec2( 1.0, -1.0));
    vec3 rgb_sw = sampleScene(uv + inv_res * vec2(-1.0,  1.0));
    vec3 rgb_se = sampleScene(uv + inv_res * vec2( 1.0,  1.0));

    float luma_m  = luma709(rgb_m);
    float luma_nw = luma709(rgb_nw);
    float luma_ne = luma709(rgb_ne);
    float luma_sw = luma709(rgb_sw);
    float luma_se = luma709(rgb_se);

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
        sampleScene(uv + dir * (1.0/3.0 - 0.5)) +
        sampleScene(uv + dir * (2.0/3.0 - 0.5)));
    vec3 rgb_b = rgb_a * 0.5 + 0.25 * (
        sampleScene(uv + dir * -0.5) +
        sampleScene(uv + dir *  0.5));

    // If the second-pass luma escapes the corner range, the edge direction
    // was too oblique — fall back to the first-pass blend.
    float luma_b = luma709(rgb_b);
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
        frag_color = vec4(sampleScene(uv), 1.0);
    }
}
@end

@program composite composite_vs composite_fs
