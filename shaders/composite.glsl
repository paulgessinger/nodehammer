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
//   3 — overdraw heatmap (scene_color.r carries an additive per-pixel
//       fragment count; mapped through a jet ramp)
// Depth and overdraw views bypass exposure and tonemap entirely.
//
// VS reads NDC positions for a fullscreen triangle from a tiny static VBO
// owned by CompositePass. Caller issues sg_draw(0, 3, 1) with that VBO bound
// at vertex_buffers[0]. We could synthesise positions from gl_VertexIndex
// instead, but on WebGL2 a draw with no enabled vertex attribute at
// location 0 forces the browser into expensive emulation — having a real
// attribute avoids that warning at the cost of one 24-byte buffer per pass.
//
// The V flip required for top-left-origin backends is applied in the FS
// based on a uniform flag (see mode_near_far.w in composite_fs).

@module composite

@vs composite_vs
in vec2 a_pos;
out vec2 v_uv;
void main() {
    // VBO holds (-1,-1), (3,-1), (-1,3) — a triangle covering [-1,1]^2 in
    // NDC. v_uv = a_pos*0.5 + 0.5 maps the visible region to [0,1]^2 with
    // [0,2] across the full triangle, so the FS samples without scaling.
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
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
    // x = enable_background (0.0 or 1.0). When 1 and a pixel's scene depth
    //     equals the clear value (no scene geometry), `sampleScene` returns
    //     the IBL prefilter cubemap sampled along the world-space view ray
    //     instead of the scene color. yzw reserved.
    vec4 background_params;
    // Inverse of the camera's view-projection matrix. Used to reconstruct
    // a world-space view direction from screen-space NDC for the background
    // dome. Computed CPU-side as glm::inverse(view_proj).
    mat4 inv_view_proj;
    // xyz = world-space camera position. w reserved.
    vec4 camera_pos;
    // Pre-tonemap "look" knobs applied in linear HDR space, after
    // exposure and before the tonemap curve.
    // x = contrast (1.0 = identity, pivot around perceptual mid-gray 0.18)
    // y = saturation (1.0 = identity, 0.0 = grayscale)
    // zw reserved.
    vec4 look_params;
    // FXAA (PC-quality variant) tunables. Appended last so existing std140
    // offsets stay stable; sokol-shdc regenerates the C struct in lockstep.
    // x = subpix amount (0..1), y = relative edge threshold,
    // z = absolute edge threshold, w = max edge-search steps.
    vec4 fxaa_quality;
    // Overdraw heatmap (mode 3) params. Appended last for std140 stability.
    // x = count scale — multiply the sampled scene_color.r (an additively
    //     accumulated, increment-scaled fragment count) by this to recover
    //     the integer draw count. = 1/increment; see overdrawColorIncrement.
    // y = range — the count that maps to the hot end of the ramp; counts
    //     above it clamp to white. zw reserved.
    vec4 overdraw_params;
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
// IBL prefilter cubemap reused as the visible background. Always bound
// (the SceneRenderer's IBL is up by the time the composite runs); gated
// by background_params.x at sample time.
layout(binding=3) uniform textureCube background_env;
layout(binding=0) uniform sampler smp_color;
@sampler_type smp_depth nonfiltering
layout(binding=1) uniform sampler smp_depth;
layout(binding=2) uniform sampler smp_ao;
layout(binding=3) uniform sampler smp_env;

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

// Jet-style ramp (blue -> cyan -> green -> yellow -> red) for the overdraw
// heatmap. Input is normalized count in [0,1]. Cheap analytic form -- no LUT
// texture -- which keeps the composite single-binding.
vec3 overdraw_heat(float t) {
    t = clamp(t, 0.0, 1.0);
    return clamp(vec3(1.5 - abs(4.0 * t - 3.0),
                      1.5 - abs(4.0 * t - 2.0),
                      1.5 - abs(4.0 * t - 1.0)),
                 0.0, 1.0);
}

// Pre-tonemap "look" pass. Operates on linear HDR data after exposure and
// before the tonemap curve, so the operator's roll-off still does its work
// on the punched-up values. Contrast pivots around 0.18 (perceptual
// mid-gray): values above 0.18 brighten and below 0.18 darken when
// contrast > 1, both pulled toward 0.18 when contrast < 1. Saturation
// lerps between rec.709 luma and color: 0 = grayscale, 1 = identity,
// >1 boosts color. Default contrast=1, saturation=1 collapses to a
// near-identity pair of ops (one pow with exponent 1, one mix at t=1).
vec3 apply_look(vec3 c, float contrast, float saturation) {
    c = max(c, vec3(0.0));
    const float mid = 0.18;
    c = pow(c / mid, vec3(contrast)) * mid;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, saturation);
    return max(c, vec3(0.0));
}

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

// True when this pixel is "background" — no scene geometry has written depth.
// The depth-clear value depends on the convention: reversed-Z clears to 0.0,
// normal-Z and log-Z clear to 1.0 (the log formula maps far→1 by construction).
//
// Use exact equality, not a tolerance. With reversed-Z and a small near plane,
// the depth distribution clusters legitimate far-plane geometry within tiny
// epsilons of 0.0, so any non-zero tolerance falsely classifies real geometry
// as background. The far_plane pad in Camera ensures geometry never sits at
// exactly the clear value.
bool isBackground(float d) {
    bool reversed = depth_params.x > 0.5 && depth_params.x < 1.5;
    float clear_d = reversed ? 0.0 : 1.0;
    return d == clear_d;
}

// Reconstruct a world-space view direction for the background dome from a
// texture-space UV. Undoes the V-flip if the backend has top-left origin
// so NDC y stays +up regardless. The cubemap sample direction is independent
// of clip-space depth, so any z works for the inverse projection.
vec3 reconstructViewDir(vec2 uv) {
    float flip_v = mode_near_far.w;
    vec2 uv_ndc = vec2(uv.x, mix(uv.y, 1.0 - uv.y, flip_v));
    vec2 ndc = uv_ndc * 2.0 - 1.0;
    vec4 cs = vec4(ndc, 0.5, 1.0);
    vec4 ws = inv_view_proj * cs;
    ws.xyz /= ws.w;
    return normalize(ws.xyz - camera_pos.xyz);
}

// Sample the offscreen scene color, apply AO, exposure, then optional tonemap.
// All color paths (passthrough + FXAA) route through here so FXAA sees the
// final LDR result and its luma assumptions (rec.709) hold even when the
// scene target is RGBA16F. AO multiplies the linear scene color *before*
// exposure so the slider stays consistent with tonemap on/off.
//
// Background dome: when enable_background is on AND the pixel has depth ==
// clear (no scene geometry), `c` is the IBL prefilter cubemap sampled along
// the world-space view ray. AO is skipped on background pixels (no surface
// to occlude). Tonemap is applied uniformly so the on-screen sky matches the
// reflected sky tonally.
vec3 sampleScene(vec2 uv) {
    vec3 c;
    bool is_bg = false;
    if (background_params.x > 0.5) {
        // textureLod (vs texture) everywhere in sampleScene: FXAA calls this
        // from inside a dynamic search loop where implicit-LOD derivatives are
        // undefined (and WGSL forbids implicit sampling in non-uniform control
        // flow). All these targets are single-mip, so LOD 0 is identical.
        float d = textureLod(sampler2D(scene_depth, smp_depth), uv, 0.0).r;
        is_bg = isBackground(d);
    }
    if (is_bg) {
        vec3 dir = reconstructViewDir(uv);
        c = textureLod(samplerCube(background_env, smp_env), dir, 0.0).rgb;
    } else {
        c = textureLod(sampler2D(scene_color, smp_color), uv, 0.0).rgb;
        if (ao_params.x > 0.5) {
            float ao = textureLod(sampler2D(ao_map, smp_ao), uv, 0.0).r;
            c *= ao;
        }
    }
    c *= tonemap_params.x;
    c = apply_look(c, look_params.x, look_params.y);
    int tm = int(tonemap_params.y + 0.5);
    if      (tm == 0) c = tonemap_aces(c);
    else if (tm == 1) c = tonemap_reinhard(c);
    else if (tm == 2) c = tonemap_agx(c);
    return c;
}

float luma709(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Perceptual luma for FXAA. sampleScene returns *linear* color (the
// tonemappers apply no OETF; the sRGB encode happens on the framebuffer
// write), but FXAA's contrast thresholds were tuned for gamma-encoded luma.
// sqrt() ≈ a gamma-2.0 encode — cheap, and enough to make the thresholds
// catch the dark/midtone edges a linear luma silently skips.
float fxaaLuma(vec2 uv) { return sqrt(luma709(sampleScene(uv))); }

// FXAA 3.11 PC ("quality") variant. A 3×3 luma neighborhood drives edge
// detection; a sub-pixel lowpass term softens shimmer; and a directional
// edge-end search walks along the edge in both directions to place the blend
// with sub-pixel accuracy (this is what the cheap "console" variant lacked,
// so near-horizontal/vertical edges stayed staircased). Every tap routes
// through sampleScene/fxaaLuma so FXAA sees the final tonemapped image.
// Tunables arrive in fxaa_quality: x = subpix, y = relative edge threshold,
// z = absolute edge threshold, w = max search steps. Step sizes are the
// canonical FXAA quality preset; the loop bound is the Ultra count
// (compile-time) with an early uniform bail for the lower presets.
const int   FXAA_MAX_STEPS = 12;
const float FXAA_STEPS[12] = float[12](
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
const float FXAA_LAST_STEP_GUESS = 8.0;

vec3 fxaa(vec2 uv, vec2 inv_res) {
    float subpix    = fxaa_quality.x;
    float edge_rel  = fxaa_quality.y;
    float edge_min  = fxaa_quality.z;
    int   max_steps = int(clamp(fxaa_quality.w, 1.0, float(FXAA_MAX_STEPS)));

    // 3×3 luma neighborhood (cardinals first — enough to early-out).
    float lumaM = fxaaLuma(uv);
    float lumaN = fxaaLuma(uv + vec2(0.0, -inv_res.y));
    float lumaS = fxaaLuma(uv + vec2(0.0,  inv_res.y));
    float lumaE = fxaaLuma(uv + vec2( inv_res.x, 0.0));
    float lumaW = fxaaLuma(uv + vec2(-inv_res.x, 0.0));

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range   = lumaMax - lumaMin;

    // Flat region (or near-black noise) — leave the pixel untouched. Everything
    // below runs only for edges. Written as a single exit with `finalUV`
    // defaulting to the source texel: a mid-function return here trips the
    // HLSL/FXC back-end's uninitialized-return check (X4000).
    vec2 finalUV = uv;
    if (range >= max(edge_min, lumaMax * edge_rel)) {
        float lumaNW = fxaaLuma(uv + vec2(-inv_res.x, -inv_res.y));
        float lumaNE = fxaaLuma(uv + vec2( inv_res.x, -inv_res.y));
        float lumaSW = fxaaLuma(uv + vec2(-inv_res.x,  inv_res.y));
        float lumaSE = fxaaLuma(uv + vec2( inv_res.x,  inv_res.y));

        // Sub-pixel term: how far the center deviates from the 3×3 lowpass,
        // smoothed, squared, and scaled by the subpix knob.
        float lumaAvg = (2.0 * (lumaN + lumaS + lumaE + lumaW) +
                         lumaNW + lumaNE + lumaSW + lumaSE) * (1.0 / 12.0);
        float subpixBlend = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
        subpixBlend = smoothstep(0.0, 1.0, subpixBlend);
        subpixBlend = subpixBlend * subpixBlend * subpix;

        // Edge orientation: weighted second derivatives along each axis.
        float edgeHorz =
            abs(lumaN  + lumaS  - 2.0 * lumaM) * 2.0 +
            abs(lumaNE + lumaSE - 2.0 * lumaE) +
            abs(lumaNW + lumaSW - 2.0 * lumaW);
        float edgeVert =
            abs(lumaE  + lumaW  - 2.0 * lumaM) * 2.0 +
            abs(lumaNE + lumaNW - 2.0 * lumaN) +
            abs(lumaSE + lumaSW - 2.0 * lumaS);
        bool horizontal = edgeHorz >= edgeVert;

        // Gradient: the steeper of the two sides perpendicular to the edge picks
        // the blend direction (one texel toward the higher-contrast neighbor).
        float lumaP = horizontal ? lumaN : lumaE;
        float lumaQ = horizontal ? lumaS : lumaW;
        float gradP = abs(lumaP - lumaM);
        float gradQ = abs(lumaQ - lumaM);

        float pixelStep = horizontal ? inv_res.y : inv_res.x;
        float oppositeLuma;
        float gradient;
        if (gradP < gradQ) {
            pixelStep   = -pixelStep;
            oppositeLuma = lumaQ;
            gradient     = gradQ;
        } else {
            oppositeLuma = lumaP;
            gradient     = gradP;
        }

        // Edge-end search: from the midline between the center and its steeper
        // neighbor, walk along the edge until the average luma diverges past a
        // quarter of the gradient (the edge end) or the step budget runs out.
        vec2 edgeUV = uv;
        vec2 stepUV = vec2(0.0);
        if (horizontal) {
            edgeUV.y += 0.5 * pixelStep;
            stepUV.x  = inv_res.x;
        } else {
            edgeUV.x += 0.5 * pixelStep;
            stepUV.y  = inv_res.y;
        }
        float edgeLuma   = 0.5 * (lumaM + oppositeLuma);
        float gradThresh = 0.25 * gradient;

        vec2  uvP   = edgeUV + stepUV * FXAA_STEPS[0];
        float dP    = fxaaLuma(uvP) - edgeLuma;
        bool  doneP = abs(dP) >= gradThresh;
        for (int i = 1; i < FXAA_MAX_STEPS; ++i) {
            if (i >= max_steps || doneP) break;
            uvP  += stepUV * FXAA_STEPS[i];
            dP    = fxaaLuma(uvP) - edgeLuma;
            doneP = abs(dP) >= gradThresh;
        }
        if (!doneP) uvP += stepUV * FXAA_LAST_STEP_GUESS;

        vec2  uvN   = edgeUV - stepUV * FXAA_STEPS[0];
        float dN    = fxaaLuma(uvN) - edgeLuma;
        bool  doneN = abs(dN) >= gradThresh;
        for (int i = 1; i < FXAA_MAX_STEPS; ++i) {
            if (i >= max_steps || doneN) break;
            uvN  -= stepUV * FXAA_STEPS[i];
            dN    = fxaaLuma(uvN) - edgeLuma;
            doneN = abs(dN) >= gradThresh;
        }
        if (!doneN) uvN -= stepUV * FXAA_LAST_STEP_GUESS;

        // Distance to each end along the edge axis; blend from the nearer end.
        float distP, distN;
        if (horizontal) { distP = uvP.x - uv.x; distN = uv.x - uvN.x; }
        else            { distP = uvP.y - uv.y; distN = uv.y - uvN.y; }

        bool  nearestIsP   = distP <= distN;
        float nearestDist  = nearestIsP ? distP : distN;
        float nearestDelta = nearestIsP ? dP : dN;
        float edgeLength   = distP + distN;

        // Only blend when the center is on the opposite side of the edge midline
        // from the divergence at the nearest end — otherwise blending would push
        // the edge the wrong way.
        float edgeBlend = 0.0;
        if (((lumaM - edgeLuma) >= 0.0) != (nearestDelta >= 0.0)) {
            edgeBlend = 0.5 - nearestDist / edgeLength;
        }

        float blend = max(subpixBlend, edgeBlend);

        if (horizontal) finalUV.y += blend * pixelStep;
        else            finalUV.x += blend * pixelStep;
    }
    return sampleScene(finalUV);
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
    } else if (mode == 3) {
        // Overdraw heatmap. scene_color.r is the additively-accumulated,
        // increment-scaled fragment count (see the scene FS overdraw path).
        // Recover the integer count, normalize by the user range, and map
        // through the jet ramp. 0 draws -> near-black; counts over the range
        // clamp to white to flag the densest hotspots.
        float count = texture(sampler2D(scene_color, smp_color), uv).r * overdraw_params.x;
        float range = max(overdraw_params.y, 1.0);
        if (count < 0.5) {
            frag_color = vec4(vec3(0.02), 1.0);
        } else if (count > range) {
            frag_color = vec4(1.0, 1.0, 1.0, 1.0);
        } else {
            frag_color = vec4(overdraw_heat(count / range), 1.0);
        }
    } else if (fxaa_params.x > 0.5) {
        frag_color = vec4(fxaa(uv, fxaa_params.yz), 1.0);
    } else {
        frag_color = vec4(sampleScene(uv), 1.0);
    }
}
@end

@program composite composite_vs composite_fs
