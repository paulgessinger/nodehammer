// Bilateral denoise pass for GTAO output. Reads the raw AO target (R = AO)
// and scene depth, writes a denoised target with the same layout.
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
// The AO scalar (R channel) is bounded in [0,1] with no encode/decode round
// trip, so the bilateral only ever smooths it. (The GTAO bent normal this
// pass used to also carry in G/B was dropped when AO consumption moved to the
// composite — see ao.glsl / composite.glsl.)

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
    // w = reserved.
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

    // Sample the center tap once up-front as a safe fallback for the AO scalar
    // if every other tap gets rejected.
    vec4 center_sample = texture(sampler2D(ao_raw, smp_ao), uv);

    float ao_sum = 0.0;
    float w_sum = 0.0;

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
            float tap_ao = texture(sampler2D(ao_raw, smp_ao), tap_uv).r;

            ao_sum += tap_ao * w;
            w_sum += w;
        }
    }

    float ao = (w_sum > 1e-6) ? (ao_sum / w_sum) : center_sample.r;
    frag_color = vec4(ao, 0.0, 0.0, 1.0);
}
@end

@program ao_denoise ao_denoise_vs ao_denoise_fs
