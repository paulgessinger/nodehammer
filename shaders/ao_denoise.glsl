// Bilateral denoise pass for GTAO output. Reads the raw RGBA8 AO target
// (R = AO, GB = octahedral bent normal in world space) and scene depth,
// writes a denoised RGBA8 target with the same layout.
//
// Why bilateral, why now: GTAO at full-res still leaves visible IGN-jitter
// noise on uniform surfaces — the per-pixel slice-phase decorrelation
// breaks up directional banding but trades it for high-frequency speckle.
// A depth-weighted bilateral averages the speckle out without crossing
// depth discontinuities (which would smear AO across silhouettes).
//
// Sample-count choice: 5×5 (25 taps) over a separable two-pass 5-tap. The
// 25-tap variant uses ~30% less bandwidth than two 5-tap passes (one
// fewer attachment write, no intermediate ping-pong) and is wide enough
// to flatten the residual speckle that a 3×3 kernel leaves on uniform
// surfaces. Cost: ~25 texture taps per pixel, which the GPU absorbs
// inside the depth-fetch latency. Promote to separable if profiling
// ever flags this as a bottleneck.
//
// Bent normal handling: averaged across the same bilateral kernel as the AO
// scalar (a weighted spherical mean — decode each tap's octahedral GB to a unit
// vector, accumulate with the tap's spatial×depth weight, renormalize, re-
// encode), BUT only when the target is high-precision (RGBA16F). On an RGBA8
// fallback the average is skipped and the center tap's bent normal is passed
// through unchanged.
//
// The precision gate matters. Averaging unit vectors then renormalizing exposes
// the storage quantization: at 8 bits per octahedral axis, adjacent output
// pixels round to *different* bins ~1° apart, which the irradiance cubemap turns
// into a fine color speckle on saturated diffuse surfaces — so at 8-bit,
// passthrough actually read cleaner than averaging. At 16F (~11 mantissa bits,
// bins ~0.05° apart) that speckle is gone, and averaging is a clear win: it
// removes the per-pixel slice-phase (IGN) jitter that otherwise shows up as
// low-frequency color mottling on flat surfaces — and which "crawls" under
// camera motion because the jitter is locked to screen space while the geometry
// moves under it. The enable flag is frame_params.w, set by the CPU from the AO
// target's pixel format.
//
// The AO *scalar* (R channel) is always averaged — it's bounded in [0,1] with
// no encode/decode round-trip, so the bilateral only ever smooths it.

@module ao_denoise

@vs ao_denoise_vs
in vec2 a_pos;
out vec2 v_uv;
void main() {
    // Same fullscreen-triangle trick as ao.glsl — see composite.glsl for
    // the rationale on a real attribute-0 buffer (vs gl_VertexIndex
    // synthesis) on WebGL2.
    v_uv = a_pos * 0.5 + 0.5;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
@end

@fs ao_denoise_fs
layout(binding=0) uniform ao_denoise_params_block {
    // x = depth_mode (0=normalZ, 1=reversedZ, 2=logZ — same convention as
    //     composite.glsl / ao.glsl).
    // y = far_plane (only read in log-Z mode).
    // z = near_plane.
    // w = sigma_z (depth-weight falloff in *linear view-space* units —
    //     larger sigma blurs across more depth difference). Default ~1%
    //     of (far - near).
    vec4 depth_params;
    // x = 1/render_width, y = 1/render_height (texel size).
    // z = flip_v (1.0 on top-left-origin backends; applied to v_uv before
    //     sampling both the raw AO target and scene depth so the read is
    //     aligned to the same pixel that produced it).
    // w = denoise_bent (1.0 = average the bent normal across the kernel; 0.0 =
    //     pass the center tap's bent normal through unchanged). Set to 1.0 only
    //     when the AO target is RGBA16F — see the file header on why averaging
    //     an 8-bit octahedral bent normal speckles.
    vec4 frame_params;
};

layout(binding=0) uniform texture2D ao_raw;
layout(binding=1) uniform texture2D scene_depth;
layout(binding=0) uniform sampler   smp_ao;
@sampler_type smp_depth nonfiltering
layout(binding=1) uniform sampler   smp_depth;
@image_sample_type scene_depth unfilterable_float

@include depth_helpers.glsl.h

in vec2 v_uv;
out vec4 frag_color;

// Octahedral encode/decode for the bent-normal spherical mean. Must match the
// producer (ao.glsl octEncode) and consumer (scene.glsl octDecodeBent) exactly
// so the GB channels survive the decode→average→re-encode round trip with the
// same convention they're read back under.
vec3 octDecode(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        vec2 sn = vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
        v.xy = (1.0 - abs(v.yx)) * sn;
    }
    return normalize(v);
}

vec2 octEncode(vec3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    vec2 e = n.xy;
    if (n.z < 0.0) {
        vec2 sn = vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
        e = (1.0 - abs(e.yx)) * sn;
    }
    return e * 0.5 + 0.5;
}

float linearZ(float d) {
    return linearize_depth(d, depth_params.z, depth_params.y,
                           depth_params.x, depth_params.y);
}

void main() {
    float flip_v = frame_params.z;
    vec2 uv = vec2(v_uv.x, mix(v_uv.y, 1.0 - v_uv.y, flip_v));

    vec2 texel = frame_params.xy;

    // Center reference values. We weight every tap against the center
    // pixel's linearized depth so the filter doesn't smear AO across
    // silhouettes (where the depth gradient is huge).
    float center_d = texture(sampler2D(scene_depth, smp_depth), uv).r;
    float center_z = linearZ(center_d);
    float sigma_z = max(depth_params.w, 1e-4);
    float inv_2_sigma_sq = 1.0 / (2.0 * sigma_z * sigma_z);

    // 5×5 spatial Gaussian weights (sigma ≈ 1.4 pixels). Computed inline
    // from the tap offset rather than table-driven so we don't pay for an
    // array indexing pattern that some WebGL2 drivers handle clumsily.
    // The bilateral depth term multiplies onto these per-tap, so the final
    // per-tap weight is `spatial * depth_weight`.
    const float SPATIAL_SIGMA = 1.4;
    const float spatial_falloff = 1.0 / (2.0 * SPATIAL_SIGMA * SPATIAL_SIGMA);

    // Sample the center tap once up-front; we use it as the bent-normal
    // fallback (RGBA8 passthrough, or a degenerate bent sum) and as a safe
    // fallback for the AO scalar if every other tap gets rejected.
    vec4 center_sample = texture(sampler2D(ao_raw, smp_ao), uv);

    bool denoise_bent = frame_params.w > 0.5;

    float ao_sum = 0.0;
    float w_sum = 0.0;
    vec3 bent_sum = vec3(0.0);

    // Loop bounds are compile-time constants so the inner loop unrolls on
    // every shader compiler we ship to. 25 taps total — bilateral 5×5 on
    // the AO scalar only. The denoise pass itself is gated by
    // `enable_ao_denoise` on the CPU side; when off, the pass is skipped
    // entirely and downstream consumers rebind to the raw target.
    for (int oy = -2; oy <= 2; ++oy) {
        for (int ox = -2; ox <= 2; ++ox) {
            vec2 off = vec2(float(ox), float(oy)) * texel;
            vec2 tap_uv = uv + off;

            float r2 = float(ox * ox + oy * oy);
            float spatial = exp(-r2 * spatial_falloff);

            float tap_d = texture(sampler2D(scene_depth, smp_depth), tap_uv).r;
            float tap_z = linearZ(tap_d);
            float dz = tap_z - center_z;
            float depth_w = exp(-(dz * dz) * inv_2_sigma_sq);

            float w = spatial * depth_w;
            vec4 tap = texture(sampler2D(ao_raw, smp_ao), tap_uv);

            ao_sum += tap.r * w;
            w_sum += w;
            // Bent normal: weighted spherical mean over the same bilateral
            // kernel (depth weight keeps it from averaging across silhouettes).
            // Skipped on the RGBA8 fallback — see the file header.
            if (denoise_bent) {
                bent_sum += octDecode(tap.gb) * w;
            }
        }
    }

    float ao = (w_sum > 1e-6) ? (ao_sum / w_sum) : center_sample.r;

    // Bent normal: re-encode the averaged direction when denoising and the sum
    // is non-degenerate; otherwise pass the center tap through unchanged.
    vec2 bent_oct = (denoise_bent && dot(bent_sum, bent_sum) > 1e-12)
                        ? octEncode(normalize(bent_sum))
                        : center_sample.gb;
    frag_color = vec4(ao, bent_oct, 1.0);
}
@end

@program ao_denoise ao_denoise_vs ao_denoise_fs
