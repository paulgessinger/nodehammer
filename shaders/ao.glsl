// Screen-space ambient occlusion (GTAO v2) — depth-only, full resolution,
// single FS pass, single-channel AO output.
//
// Reference: Jiménez et al. 2016, "Practical Realtime Strategies for Accurate
// Indirect Occlusion" (Activision). Horizon-based, 4 slices uniformly
// rotated, 4 steps per slice in each direction, integrating a cosine-
// weighted horizon angle around the surface normal.
//
// Inputs: scene depth view from SceneRenderTarget. Reconstructs view-space
// position from depth + projection params and face normal from screen-space
// derivatives of view-space position (no normal target — derivatives are
// noisier on uniform surfaces but the bilateral denoise pass downstream
// hides residual jitter).
//
// Output: R = ambient occlusion (1.0 = unoccluded, 0.0 = fully occluded);
// G/B/A unused. The composite pass consumes R as a current-frame scalar
// multiply (see composite.glsl). The GTAO bent normal this pass used to also
// produce was dropped when AO consumption moved out of the scene shader.
//
// Backend caveat: dFdx/dFdy are core-in-FS at `#version 300 es` (WebGL2)
// and on every other slang sokol-shdc emits, so no extension declaration
// is needed. sokol-shdc emits the version directive automatically.

@module ao

@vs ao_vs
in vec2 a_pos;
out vec2 v_uv;
void main() {
    // VBO holds (-1,-1), (3,-1), (-1,3). Real attribute (vs synthesising
    // from gl_VertexIndex) so WebGL2 doesn't fall into emulation for an
    // unenabled vertex-attrib-0 array. See composite.glsl for the rationale.
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
@end

@fs ao_fs
layout(binding=0) uniform ao_params_block {
    // For perspective:  x = tan(fov/2) * aspect,  y = tan(fov/2).
    // For orthographic: x = ortho_half_width,     y = ortho_half_height.
    //   In ortho the per-pixel ray direction is constant; view-space x/y do
    //   not scale with depth. The shader picks the right reconstruction
    //   from `proj_params.x` (perspective flag).
    // zw reserved.
    vec4 inv_proj;
    // x = depth_mode (0=normalZ, 1=reversedZ, 2=logZ — same convention as
    //     composite.glsl's depth_params).
    // y = far_plane (only read in log-Z mode).
    // z = near_plane.
    // w = reserved.
    vec4 depth_params;
    // x = intensity (slider; applied as pow(ao, intensity)).
    // y = radius (world units; the ray-march stride is scaled to keep
    //     on-screen sample spread bounded across depth).
    // z = 1/render_width, w = 1/render_height (texel size).
    vec4 ao_params;
    // Sample-count control, set from `RenderQualitySettings::ao_quality`.
    // x = num_slices (integer in [1, NUM_SLICES_MAX])
    // y = num_steps  (integer in [1, NUM_STEPS_MAX])
    // The shader loops up to the compile-time maxima and breaks once the
    // current iteration exceeds the per-frame values — keeps loop bounds
    // constant for old shader compilers while still letting a runtime
    // uniform drive the effective count.
    // z/w reserved.
    vec4 ao_quality;
    // x = flip_v (1.0 on top-left-origin backends — applied to v_uv before
    //     sampling depth; the AO target is consumed by the composite which
    //     applies the same flip to scene_color so they stay co-aligned).
    // y = is_perspective (1.0 perspective, 0.0 orthographic).
    // z = thickness (max length of the (sample - center) horizon vector
    //     before the sample is rejected as "different surface" — kills
    //     silhouette fringes around the scene's outline).
    // w = reserved.
    vec4 frame_params;
};

layout(binding=0) uniform texture2D scene_depth;
@sampler_type smp_depth nonfiltering
layout(binding=0) uniform sampler smp_depth;

@image_sample_type scene_depth unfilterable_float

@include depth_helpers.glsl.h

in vec2 v_uv;
out vec4 frag_color;

const float PI = 3.14159265358979323846;
// Compile-time caps on the loop bounds. The Ultra quality preset sets the
// per-frame counts to these values; lower presets break out of the loops
// early via the `num_slices` / `num_steps` uniforms. Static caps keep the
// for-loop bounds constant for shader compilers that don't unroll runtime
// loops well (older WebGL2 drivers in particular).
const int NUM_SLICES_MAX = 8;
const int NUM_STEPS_MAX  = 8;

// Reconstruct the view-space position of a pixel given its UV and the
// linearized view Z. Camera looks down -Z in view space.
//
// Perspective: view-space x/y scale linearly with depth (frustum widens) —
//   x = ndc.x * tan(fov/2) * aspect * z.
// Orthographic: view-space x/y are independent of depth (parallel rays) —
//   x = ndc.x * half_width.
// `frame_params.y` selects between the two.
vec3 viewPosFromUV(vec2 uv, float linear_z) {
    vec2 ndc = uv * 2.0 - 1.0;
    bool is_perspective = frame_params.y > 0.5;
    vec2 xy = is_perspective ? (ndc * inv_proj.xy * linear_z) : (ndc * inv_proj.xy);
    return vec3(xy, -linear_z);
}

bool isFarSample(float d) {
    // Reversed-Z: d=0 at far. Normal-Z / log-Z: d=1 at far.
    return (depth_params.x > 0.5 && depth_params.x < 1.5)
        ? (d <= 1e-5)
        : (d >= 1.0 - 1e-5);
}

// Cheap per-pixel rotation phase. Jorge Jiménez's interleaved gradient noise
// (Activision 2014) — three magic constants, returns a value in roughly
// [0, 1) that decorrelates neighboring pixels enough to break up slice-
// direction banding without a temporal accumulator.
float ign(vec2 px) {
    return fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));
}

void main() {
    float flip_v = frame_params.x;
    vec2 uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, flip_v));

    // Center sample. Reject background — depth at the far plane has no
    // meaningful occluders, so write fully-unoccluded and bail.
    float center_d = texture(sampler2D(scene_depth, smp_depth), uv).r;
    if (isFarSample(center_d)) {
        frag_color = vec4(1.0, 0.0, 0.0, 1.0);
        return;
    }
    float center_z = linearize_depth(center_d, depth_params.z, depth_params.y,
                                     depth_params.x, depth_params.y);
    vec3 P = viewPosFromUV(uv, center_z);

    // Reconstruct face normal from cardinal depth taps with min-difference
    // selection (Improved Normal Reconstruction from Depth, Wu/Karis 2014).
    // dFdx/dFdy go unstable at grazing angles where adjacent quads see large
    // depth jumps that flip sign — those produce visible per-quad stripes
    // on foreshortened surfaces. Picking the side with smaller depth delta
    // for each axis keeps the normal stable across silhouettes too: a tap
    // that lands on the background gets discarded in favor of the in-surface
    // side.
    // Normal-reconstruction tap offset. Use a *fixed screen-space* size (a small
    // fraction of the frame), not ±1 AO texel. The reconstructed normal — and
    // therefore the bent normal and the AO it drives — must not depend on the
    // resolution AO happens to be computed at, or it visibly shifts ("pops") when
    // the dynamic render scale jumps on settle: a ±1-texel baseline shrinks/grows
    // with the scale, tilting the normal differently and re-tinting the bent-
    // normal irradiance. A fixed UV baseline makes the estimate scale-invariant.
    // Floored at the AO texel so the two taps never collapse into one texel under
    // nearest depth sampling (which would give a degenerate gradient).
    const float NORMAL_RECON_UV = 0.0015;
    vec2 texel = max(vec2(NORMAL_RECON_UV), ao_params.zw);
    float d_l = texture(sampler2D(scene_depth, smp_depth), uv - vec2(texel.x, 0.0)).r;
    float d_r = texture(sampler2D(scene_depth, smp_depth), uv + vec2(texel.x, 0.0)).r;
    float d_d = texture(sampler2D(scene_depth, smp_depth), uv - vec2(0.0, texel.y)).r;
    float d_u = texture(sampler2D(scene_depth, smp_depth), uv + vec2(0.0, texel.y)).r;
    // If a cardinal tap is on the sky, treat it as a wall by using center_z —
    // this throws the depth delta to "infinity" so the *other* side is
    // automatically picked by the min-difference logic below.
    float z_l = isFarSample(d_l) ? center_z + 1e9
              : linearize_depth(d_l, depth_params.z, depth_params.y,
                                depth_params.x, depth_params.y);
    float z_r = isFarSample(d_r) ? center_z + 1e9
              : linearize_depth(d_r, depth_params.z, depth_params.y,
                                depth_params.x, depth_params.y);
    float z_dn = isFarSample(d_d) ? center_z + 1e9
               : linearize_depth(d_d, depth_params.z, depth_params.y,
                                 depth_params.x, depth_params.y);
    float z_up = isFarSample(d_u) ? center_z + 1e9
               : linearize_depth(d_u, depth_params.z, depth_params.y,
                                 depth_params.x, depth_params.y);
    vec3 P_l = viewPosFromUV(uv - vec2(texel.x, 0.0), z_l);
    vec3 P_r = viewPosFromUV(uv + vec2(texel.x, 0.0), z_r);
    vec3 P_dn = viewPosFromUV(uv - vec2(0.0, texel.y), z_dn);
    vec3 P_up = viewPosFromUV(uv + vec2(0.0, texel.y), z_up);
    vec3 dx = (abs(z_l - center_z) < abs(z_r - center_z)) ? (P - P_l) : (P_r - P);
    vec3 dy = (abs(z_dn - center_z) < abs(z_up - center_z)) ? (P - P_dn) : (P_up - P);
    vec3 N = normalize(cross(dy, dx));

    // View vector. In ortho V is constant +Z (camera looks down -Z, viewer
    // direction toward eye is +Z); in perspective V varies per pixel.
    bool is_perspective = frame_params.y > 0.5;
    vec3 V = is_perspective ? normalize(-P) : vec3(0.0, 0.0, 1.0);

    // Orient the reconstructed normal toward the viewer. AO is computed for
    // the surface we can actually see (the depth buffer only holds visible
    // fragments), so the relevant hemisphere is the one around that surface's
    // camera-facing normal. The GTAO integrand below integrates *around N* and
    // so depends on N's sign — a stable, correct orientation is required.
    //
    // The test is `dot(N, V) < 0`, NOT the old `N.z < 0`. `N.z` is the normal's
    // view-space Z, which only equals "faces away from the camera" when the eye
    // looks straight down view-Z. In perspective, a clearly front-facing quad
    // off to the side of the screen can have N.z < 0 while dot(N, V) > 0 — so
    // `N.z < 0` flipped visible surfaces, and because a flat quad shares one N
    // the whole quad flipped at once mid-orbit, snapping its AO and bent normal.
    // `dot(N, V)` flips only at the true silhouette (edge-on), where the quad is
    // a sliver on screen and the transition is imperceptible.
    if (dot(N, V) < 0.0) {
        N = -N;
    }

    // Step length in UV space.
    //   Perspective: world radius shrinks on screen as the fragment recedes;
    //     scale by tan(fov/2)*|z| so on-screen spread stays bounded.
    //   Orthographic: pixel size is constant in world space across depth;
    //     the ratio is just radius / (2 * ortho_half_height) per V step.
    float radius_world = ao_params.y;
    float radius_uv;
    if (is_perspective) {
        radius_uv = radius_world / max(abs(center_z) * inv_proj.y * 2.0, 1e-4);
    } else {
        radius_uv = radius_world / max(inv_proj.y * 2.0, 1e-4);
    }
    // Tight clamp: at extreme zoom an unbounded step would land 4 taps far
    // off the surface and bake the slice direction into the output. Cap at
    // ~5% of the screen so banding stays sub-perceptual at any zoom.
    radius_uv = clamp(radius_uv, ao_params.z * 2.0, 0.05);

    // Per-frame loop counts (from the AO quality preset). Clamp to the
    // compile-time caps and to a minimum of 1 so the inner loops do at
    // least one iteration even if a bogus value sneaks through.
    int num_slices = int(clamp(ao_quality.x, 1.0, float(NUM_SLICES_MAX)));
    int num_steps  = int(clamp(ao_quality.y, 1.0, float(NUM_STEPS_MAX)));
    float step_len = radius_uv / float(num_steps);

    // Per-pixel slice rotation. Without this the hardcoded slice azimuths
    // bake into visible streaks on uniform surfaces at large radius_uv. IGN
    // is cheap and decorrelates neighbors enough that the bilateral denoise
    // downstream cleans up the residual noise.
    float jitter_phi = ign(gl_FragCoord.xy) * (PI / float(num_slices));

    // Thickness cap: reject horizon samples whose perpendicular distance to
    // the local tangent plane exceeds `thickness * radius_world`. Testing
    // tangent-plane offset (rather than total `length(H)`) is critical on
    // grazing surfaces — samples there can be far in the *tangent* direction
    // while still belonging to the same surface, and a length-based reject
    // would zero out AO across most of the wedge. The perpendicular test
    // catches genuine "different surface" cases (silhouette background,
    // overlapping geometry) without false-rejecting legitimate horizons.
    float thickness = max(frame_params.z, 1e-4) * radius_world;

    float occlusion = 0.0;

    for (int s = 0; s < NUM_SLICES_MAX; ++s) {
        if (s >= num_slices) break;
        float phi = (float(s) + 0.5) * (PI / float(num_slices)) + jitter_phi;
        vec2 dir = vec2(cos(phi), sin(phi));

        // Slice basis. The slice plane contains V and `slice_dir` (the
        // screen-space sampling direction in view space). `slice_axis` is that
        // plane's normal; `omega` is the in-plane direction perpendicular to V
        // pointing toward +slice_dir — the axis the horizon angle is measured
        // along. The projected-normal angle below lives in the orthonormal
        // (V, omega) basis of this plane.
        vec3 slice_dir = vec3(dir.x, dir.y, 0.0);
        vec3 slice_axis = normalize(cross(slice_dir, V));
        vec3 omega = normalize(cross(V, slice_axis));

        // Walk +dir and -dir; track the maximum cosine of the angle between V
        // and the ray to each sample (= horizon angle's cos) on each side.
        // Reject samples that hit the sky (no occluder there) or land
        // farther than `thickness` away (different surface, not a horizon).
        float max_cos_pos = -1.0;
        float max_cos_neg = -1.0;
        for (int i = 1; i <= NUM_STEPS_MAX; ++i) {
            if (i > num_steps) break;
            vec2 du = dir * step_len * float(i);

            vec2 uv_p = uv + du;
            // Explicit LOD 0: sampling inside this varying-count loop with an
            // implicit-derivative texture() forces the HLSL/FXC back-end to
            // unroll (X3570). The depth buffer is single-mip, so this is exact.
            float dp = textureLod(sampler2D(scene_depth, smp_depth), uv_p, 0.0).r;
            if (!isFarSample(dp)) {
                float zp = linearize_depth(dp, depth_params.z, depth_params.y,
                                           depth_params.x, depth_params.y);
                vec3 H_p = viewPosFromUV(uv_p, zp) - P;
                float l_p = length(H_p);
                if (l_p > 1e-5 && abs(dot(H_p, N)) < thickness) {
                    float c = dot(H_p / l_p, V);
                    max_cos_pos = max(max_cos_pos, c);
                }
            }

            vec2 uv_n = uv - du;
            float dn = textureLod(sampler2D(scene_depth, smp_depth), uv_n, 0.0).r;
            if (!isFarSample(dn)) {
                float zn = linearize_depth(dn, depth_params.z, depth_params.y,
                                           depth_params.x, depth_params.y);
                vec3 H_n = viewPosFromUV(uv_n, zn) - P;
                float l_n = length(H_n);
                if (l_n > 1e-5 && abs(dot(H_n, N)) < thickness) {
                    float c = dot(H_n / l_n, V);
                    max_cos_neg = max(max_cos_neg, c);
                }
            }
        }

        // GTAO arc integral (Jiménez 2016 §3.2, after XeGTAO), integrating
        // visibility over the hemisphere around the *surface normal* — not
        // just the view vector.
        //
        // This is the fix for the "AO glow" on large flat faces. The previous
        // integrand `1 - 0.5*(t_pos² + t_neg²)` only ever referenced V. On a
        // flat surface seen off-axis, coplanar up-slope samples have H·V > 0
        // and so registered as false horizons, dropping visibility as the
        // grazing angle grew. Across a flat face in perspective the grazing
        // angle is smallest where the surface crosses screen-center and largest
        // at the edges, painting a soft radial brightening onto the face.
        // Integrating around N removes it: a fully unoccluded surface
        // integrates to 1 at any view angle.
        //
        // gamma is the signed angle of N projected into the slice plane,
        // measured from V toward omega. projN_len is that projection's length
        // and is the per-slice weight (it down-weights slices the normal barely
        // lies in). Because N was oriented toward V above, NoV ≥ 0 and
        // gamma ∈ (-π/2, π/2).
        float NoV = dot(N, V);
        float NoW = dot(N, omega);
        float projN_len = sqrt(max(NoV * NoV + NoW * NoW, 1e-8));
        float gamma = atan(NoW, NoV);
        float cos_gamma = NoV / projN_len;   // = cos(gamma)
        float sin_gamma = NoW / projN_len;   // = sin(gamma)

        // Raw horizon angles from V. When no occluder was found on a side,
        // max_cos_* is still its -1 init → acos = π (fully open). Do NOT clamp
        // the cosine to [0,1]: that would cap the horizon at π/2 from V and
        // truncate the open arc on the up-tilt side. Instead bend each horizon
        // into [gamma - π/2, gamma + π/2] so the integrated arc stays within
        // the surface's own hemisphere — that clamp is what makes a flat face
        // integrate to 1 at any view angle.
        float h_pos = acos(clamp(max_cos_pos, -1.0, 1.0));   // +omega side
        float h_neg = acos(clamp(max_cos_neg, -1.0, 1.0));   // -omega side
        float H2 = gamma + min( h_pos - gamma,  0.5 * PI);
        float H1 = gamma + max(-h_neg - gamma, -0.5 * PI);

        // Closed-form cosine-weighted visibility of each clamped arc.
        float arc2 = cos_gamma + 2.0 * H2 * sin_gamma - cos(2.0 * H2 - gamma);
        float arc1 = cos_gamma + 2.0 * H1 * sin_gamma - cos(2.0 * H1 - gamma);
        float slice_vis = 0.25 * (arc1 + arc2);

        occlusion += projN_len * slice_vis;
    }

    // Normalize by slice count (XeGTAO convention): the projN_len weighting is
    // the per-slice spatial weight, and averaging projN_len·slice_vis over a
    // uniform set of slice azimuths converges to the cosine-weighted AO.
    float ao = occlusion / float(num_slices);
    ao = clamp(ao, 0.0, 1.0);
    ao = pow(ao, max(ao_params.x, 1e-3));

    // R8 storage gives 256 levels — a smooth AO gradient over a uniform
    // surface visibly bands at ~5–6 step transitions. Bayer 4×4 ordered
    // dither converts the quantization staircase into a high-frequency
    // noise pattern that the eye averages out.
    const float bayer4[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    ivec2 px = ivec2(gl_FragCoord.xy);
    float bias = bayer4[(px.y & 3) * 4 + (px.x & 3)] - 0.5;
    ao = clamp(ao + bias * (1.0 / 255.0), 0.0, 1.0);

    frag_color = vec4(ao, 0.0, 0.0, 1.0);
}
@end

@program ao ao_vs ao_fs
