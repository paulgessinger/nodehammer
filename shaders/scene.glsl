// Scene shader for the sokol viewer. Annotated GLSL — sokol-shdc parses the
// @vs / @fs / @program blocks below and emits a generated C header with
// shader source / bytecode for every requested target language plus
// reflection structs (sg_shader_desc, attribute slots, uniform-block sizes).
//
// One uniform block per stage — sokol_gfx wants vs and fs uniforms in
// separate blocks. The vs block carries the camera matrix; the fs block
// carries the per-draw material colour, global light direction, and optional
// azimuthal cut parameters.
//
// Per-instance world matrix lives in vertex attributes inst0..inst3 with
// step rate "instance". scene_renderer.cpp populates a second vertex buffer
// of mat4s and binds it to slot 1.

@module scene

@vs scene_vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;
    // x = log_depth_enable (0.0 or 1.0), y = far_plane (only read when
    // log depth is on), zw reserved. Appended after view_proj to keep
    // existing offsets stable under std140.
    vec4 depth_params;
};

in vec3 a_position;
in vec3 a_normal;
in vec4 inst0;
in vec4 inst1;
in vec4 inst2;
in vec4 inst3;
// Per-instance LOD cross-fade factor: x = fade (0 = full detail, 1 = full
// hull proxy); yzw reserved. Computed per frame on the CPU from the
// instance's projected screen size and uploaded alongside the world matrix.
in vec4 inst_lod;

out vec3 v_normal_world;
out vec3 v_world_pos;
out float v_lod_fade;

void main() {
    v_lod_fade = inst_lod.x;
    vec4 world_pos =
          a_position.x * inst0
        + a_position.y * inst1
        + a_position.z * inst2
        +                inst3;
    v_world_pos = world_pos.xyz;
    gl_Position = view_proj * world_pos;

    // Logarithmic depth (used on GLES3 — see useLogDepth in C++). Replaces
    // the standard perspective-divide depth with a log distribution that
    // gives near-uniform precision across the entire near→far range,
    // regardless of clip-space depth convention. We compute z in NDC
    // (`[-1, 1]` for GL/GLES, `[0, 1]` for D3D/Metal/WGPU/Vulkan would
    // need a different remap, but log depth is GLES3-only today). The
    // multiply by w is undone by the perspective divide so depth ends up
    // log-distributed in `[-1, 1]`.
    if (depth_params.x > 0.5) {
        float w = gl_Position.w;
        float fc = 2.0 / log2(depth_params.y + 1.0);
        gl_Position.z = (log2(max(1e-6, 1.0 + w)) * fc - 1.0) * w;
    }

    v_normal_world =
          a_normal.x * inst0.xyz
        + a_normal.y * inst1.xyz
        + a_normal.z * inst2.xyz;
}
@end

@fs scene_fs
layout(binding=1) uniform fs_params {
    vec4 base_color;   // rgb = albedo, a = opacity
    vec4 light_dir;    // xyz = direction TO light, world space; w = intensity (PBR only)
    // x = angle-cut enabled, y = large cut flag,
    // z = LOD cross-fade dither role (0 = off, 1 = detail half, 2 = hull half)
    //     -- see the screen-door discard in main(); w = unused.
    vec4 cut_params;
    vec4 cut_start;    // xy = unit vector at start phi
    vec4 cut_end;      // xy = unit vector at end phi
    vec4 material_mr;  // x = metallic, y = roughness, z/w = unused
    // x = pbr_enable, y = prefilter_max_lod,
    // z = overdraw_increment (0 = off; >0 = emit this constant per fragment
    //     for the overdraw debug view, which runs this shader under an
    //     additive-blend / no-depth pipeline),
    // w = material_stack_prefilter enable (0/1) -- runtime toggle for the
    //     sampling-stack AA blend (see stack_prefilter below).
    vec4 mode_flags;
    vec4 camera_pos;   // xyz = world-space camera position
    vec4 emissive;     // xyz = emissive factor (linear, can be > 1.0), w = unused
    vec4 alpha_params; // x = alpha_mode (0 = OPAQUE, 1 = MASK), y = alpha_cutoff, z/w = unused
    // AO history (frame-late) for PBR's IBL term.
    //   x = enable (0/1). 0 makes the shader fall back to no-AO defaults
    //       (ao=1, bent_n=n, no multi-bounce, no specular occlusion).
    //   y = 1 / screen_width  — used to convert gl_FragCoord to texture UV
    //   z = 1 / screen_height
    //   w = bent_strength in [0,1]. 0 → use N for irradiance (no bent
    //       normal effect, but multi-bounce + SO still apply). 1 → use
    //       the raw bent normal as sampled. Lerped per-pixel.
    // The AO+bent-normal target is *previous frame's* denoised output —
    // see app.cpp for the lifecycle. 1-frame lag is invisible in a viewer
    // where the camera is mostly still, and the alternative (in-frame AO
    // before forward shading) would require a depth prepass.
    vec4 ao_history_params;
    // Material-stack prefilter (viewer AA for sampling stacks). Populated per
    // merged-stack mesh from MeshAsset::stackAverage.
    //   xyz = area-weighted average base color (linear)
    //   w   = characteristic band width (world units); 0 = mesh not tagged,
    //         prefilter skipped. Enable gate is mode_flags.w.
    vec4 stack_prefilter;
};

layout(binding=0) uniform textureCube tex_irradiance;
layout(binding=1) uniform textureCube tex_prefilter;
layout(binding=2) uniform texture2D   tex_brdf_lut;
// RGBA8 AO+bent-normal map (R = AO, GB = octahedral world-space bent normal).
// See ao.glsl / ao_denoise.glsl for the channel contract.
layout(binding=3) uniform texture2D   tex_ao_history;
layout(binding=0) uniform sampler     smp_cube;
layout(binding=1) uniform sampler     smp_lut;
layout(binding=2) uniform sampler     smp_ao_history;

in vec3 v_normal_world;
in vec3 v_world_pos;
in float v_lod_fade;
out vec4 frag_color;

const float PI = 3.14159265358979323846;

// Blue-noise-style dither threshold in [0,1) from screen pixel coords, via
// interleaved gradient noise (IGN, Jimenez). Its energy sits in high spatial
// frequencies with no repeating structure, so the LOD cross-fade stipple reads
// as fine even grain that AA erases readily -- unlike a 4x4 ordered (Bayer)
// matrix, whose regular checkerboard survives into the visible frame at the
// lower effective resolution web backends settle to. Still a stable, purely
// spatial pattern for a given pixel (no temporal component), so it holds steady
// under the viewer's pause_when_static settle.
float ignDither(vec2 frag) {
    return fract(52.9829189 * fract(dot(frag, vec2(0.06711056, 0.00583715))));
}

float D_GGX(float NdotH, float a2) {
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_SmithSchlickGGX(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float gv = NdotV / (NdotV * (1.0 - k) + k);
    float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

vec3 F_Schlick(float VdotH, vec3 F0) {
    float f = pow(1.0 - VdotH, 5.0);
    return F0 + (1.0 - F0) * f;
}

// Octahedral decode (inverse of the encode in ao.glsl / ao_denoise.glsl).
// 8-bit-per-axis input from the AO history texture; output is a unit vector
// in world space (the bent normal).
vec3 octDecodeBent(vec2 e) {
    e = e * 2.0 - 1.0;
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (v.z < 0.0) {
        vec2 sn = vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0);
        v.xy = (1.0 - abs(v.yx)) * sn;
    }
    return normalize(v);
}

// Jiménez 2016 §5.3 multi-bounce approximation. AO baked from depth alone
// over-darkens saturated diffuse materials in cavities because real bounce
// light reintroduces color-matched fill. The cubic per-channel polynomial
// `((v*a + b)*v + c)*v` (parameterized by albedo) gives a closed-form
// approximation of how much energy comes back from one bounce. `max(v, …)`
// clamps to single-bounce visibility as a floor so the approximation never
// darkens *less* than physically motivated.
vec3 gtaoMultiBounce(float v, vec3 albedo) {
    vec3 a = 2.0404 * albedo - vec3(0.3324);
    vec3 b = -4.7951 * albedo + vec3(0.6417);
    vec3 c = 2.7552 * albedo + vec3(0.6903);
    return max(vec3(v), ((v * a + b) * v + c) * v);
}

// Lagarde / "Moving Frostbite to PBR" specular occlusion fit. Approximates
// the visibility integral for the specular cone using just AO and NdotV.
// Without it, IBL reflections leak through cavities — bright sky reflected
// in a corner that's clearly in shadow. The pow exponent of 4 is the
// commonly-tuned default; values that high concentrate the effect on
// grazing pixels where the artifact is most visible.
float specularOcclusion(float NdotV, float ao) {
    return clamp(pow(NdotV + ao, 4.0) - 1.0 + ao, 0.0, 1.0);
}

void main() {
    if (cut_params.x > 0.5) {
        vec2 p = v_world_pos.xy;
        float side_start = cut_start.x * p.y - cut_start.y * p.x;
        float side_end = cut_end.x * p.y - cut_end.y * p.x;
        bool in_cut = cut_params.y > 0.5
            ? (side_start >= 0.0 || side_end <= 0.0)
            : (side_start >= 0.0 && side_end <= 0.0);
        if (in_cut) {
            discard;
        }
    }

    if (alpha_params.x > 0.5 && base_color.a < alpha_params.y) {
        discard;
    }

    // LOD cross-fade (screen-door dither). A stack in the transition band draws
    // both its detailed slabs and its hull proxy; each keeps a complementary
    // half of the pixels against a shared ordered-dither threshold, so exactly
    // one representation survives per pixel and the stack dissolves smoothly
    // detail<->hull with no pop. v_lod_fade: 0 = full detail, 1 = full hull.
    // cut_params.z selects the half (1 = detail, 2 = hull); 0 disables (Normal
    // groups and instances outside the band, which the CPU already emits to a
    // single group). Kept before the derivative-based prefilter below, matching
    // the existing cut/alpha discards.
    if (cut_params.z > 0.5) {
        float lod_thr = ignDither(gl_FragCoord.xy);
        if (cut_params.z < 1.5) {
            // Detail half: drop detail pixels as the hull fades in.
            if (v_lod_fade > lod_thr) {
                discard;
            }
        } else {
            // Hull half: the complement of the detail test.
            if (v_lod_fade <= lod_thr) {
                discard;
            }
        }
    }

    // Overdraw debug: the pipeline binds additive blending with the depth
    // test off, so every covered fragment adds `increment` to the color
    // target and the composite recovers a per-pixel draw count. Emit and
    // bail before any lighting. Placed after the cut / alpha-mask discards
    // so cut-away and masked fragments don't inflate the count.
    if (mode_flags.z > 0.0) {
        frag_color = vec4(mode_flags.z, 0.0, 0.0, mode_flags.z);
        return;
    }

    vec3 n = normalize(v_normal_world);

    // Material-stack prefilter: band-limit the cycling slab colors by blending
    // the albedo toward the stack's area-weighted average (stack_prefilter.xyz)
    // once the pixel footprint on the surface grows past the band width
    // (stack_prefilter.w) -- i.e. when the bands can no longer be resolved and
    // point sampling would alias into moire. The footprint is the world-space
    // size of the pixel projected onto the surface (from position derivatives),
    // so it grows with distance and at grazing angles -- exactly where the
    // moire is worst. Gated by the runtime toggle (mode_flags.w); untagged
    // meshes carry w = 0 and are skipped. Uniform control flow (both guards are
    // per-draw uniforms), so the derivatives are well-defined.
    vec3 albedo = base_color.rgb;
    float prefilter_t = 0.0;
    if (mode_flags.w > 0.5 && stack_prefilter.w > 0.0) {
        vec3 dpdx = dFdx(v_world_pos);
        vec3 dpdy = dFdy(v_world_pos);
        float footprint = sqrt(length(cross(dpdx, dpdy)));
        prefilter_t = smoothstep(stack_prefilter.w, 3.0 * stack_prefilter.w, footprint);
        albedo = mix(albedo, stack_prefilter.xyz, prefilter_t);
    }

    if (mode_flags.x < 0.5) {
        // Lambert (legacy fast path) — preserved byte-for-byte.
        // Two-sided shading: detector inner/outer faces light regardless of winding.
        float ndl = max(abs(dot(n, -light_dir.xyz)), 0.0);
        vec3 rgb = albedo * (0.20 + 0.80 * ndl) + emissive.xyz;
        frag_color = vec4(rgb, base_color.a);
        return;
    }

    // ---- PBR (glTF 2.0 metallic-roughness) ----
    vec3 V = normalize(camera_pos.xyz - v_world_pos);

    // Two-sided shading needs *some* notion of "the side facing the
    // viewer," but we deliberately keep two normals around:
    //   * `n_geom` — the unflipped geometric normal. Used for the diffuse
    //     IBL lookup below, where we sample the irradiance cubemap at both
    //     `+lookup_dir` and `-lookup_dir` and blend by the facing weight.
    //     The smooth blend means there's no abrupt color change at the
    //     dot(n, V) = 0 silhouette — fixes the long-standing "AO color
    //     snaps across a curved surface at the silhouette" artifact.
    //   * `n_shade` — flipped to always face the viewer. Used for the
    //     analytical Lo term (so inner detector faces still light) and
    //     for the specular reflection vector (so specular shows on the
    //     visible side). The hard flip in `n_shade` is still discontinuous
    //     at the silhouette, but Lo and specular both vanish there
    //     (NdotL → 0, specular falls off), so the visible artifact in
    //     those terms is much smaller than in diffuse IBL.
    vec3 n_geom = n;
    float face_dot = dot(n_geom, V);
    vec3 n_shade = face_dot < 0.0 ? -n_geom : n_geom;

    vec3 L = normalize(-light_dir.xyz);
    vec3 H = normalize(V + L);

    float NdotV = max(dot(n_shade, V), 1e-4);
    float NdotL = max(dot(n_shade, L), 0.0);
    float NdotH = max(dot(n_shade, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float metallic  = clamp(material_mr.x, 0.0, 1.0);
    float roughness = clamp(material_mr.y, 0.04, 1.0);

    // ---- Specular anti-aliasing: raise roughness where sub-pixel detail
    // makes the specular lobe alias, from two complementary signals ----
    // (1) Stack prefilter: as the cycling bands blur toward their average
    //     (prefilter_t -> 1), roughen toward matte so the highlight broadens
    //     and stops glinting. The footprint-average of a micro-structured
    //     glossy stack is duller, not just color-averaged.
    roughness = mix(roughness, 1.0, prefilter_t);
    // (2) Geometric specular AA (Kaplanyan/Karis): where the surface normal
    //     varies fast across the pixel (fine facets, edges), widen roughness
    //     to band-limit the BRDF. Distance-independent, so it also kills
    //     close-range highlight moire the distance-based prefilter leaves.
    vec3 dNdx = dFdx(v_normal_world);
    vec3 dNdy = dFdy(v_normal_world);
    float normalVar = dot(dNdx, dNdx) + dot(dNdy, dNdy);
    float kernelRoughness = min(2.0 * normalVar, 0.18);
    roughness = clamp(sqrt(roughness * roughness + kernelRoughness), 0.04, 1.0);

    float a = roughness * roughness;
    float a2 = a * a;

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 F  = F_Schlick(VdotH, F0);
    float D = D_GGX(NdotH, a2);
    float G = G_SmithSchlickGGX(NdotV, NdotL, roughness);

    vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kd * albedo / PI;

    vec3 Lo = (diffuse + specular) * NdotL * light_dir.w;

    // Image-based lighting (split-sum approximation) — always on in PBR mode.
    // Layered AO consumption (frame-late, from the previous frame's denoised
    // GTAO output):
    //   * `ao` modulates the *diffuse* IBL term via gtaoMultiBounce, which
    //     reintroduces color-matched bounce energy in cavities.
    //   * `bent_n` replaces `n` as the sampling direction for the irradiance
    //     cubemap — cavities pull their fill light from the direction
    //     they're open to, not from the wall they face into.
    //   * `specularOcclusion(NdotV, ao)` attenuates the *specular* IBL term
    //     so reflections don't leak through cavities.
    //
    // First frame (and any frame the History target is invalid) gets
    // ao_history_params.x == 0 and the shader collapses to no-AO defaults.
    //
    // The lookup direction is built in *unflipped* (geometric) space:
    // `n_geom` is the geometric normal as the surface defines it (may point
    // away from V on backfacing pixels of two-sided geometry), and
    // `bent_raw` is the GTAO bent normal in world space. Neither gets a
    // V-sign flip here — the smooth two-sided IBL blend below absorbs
    // whatever sign the lookup direction has without introducing a hard
    // discontinuity at the silhouette.
    float ao = 1.0;
    vec3 ibl_lookup_dir = n_geom;
    if (ao_history_params.x > 0.5) {
        vec2 ao_uv = gl_FragCoord.xy * ao_history_params.yz;
        vec4 ao_sample = texture(sampler2D(tex_ao_history, smp_ao_history), ao_uv);
        ao = ao_sample.r;
        vec3 bent_raw = octDecodeBent(ao_sample.gb);
        // No `if (dot(bent_raw, V) < 0) flip` here — that flip was the
        // second contributor to the silhouette color snap. GTAO produces a
        // camera-facing bent normal by construction (per-slice integration
        // accumulates V*cos(theta_mid) + slice_dir*sin(theta_mid), and the
        // V component is always positive for theta_mid ∈ (-π/2, π/2)), so
        // the defensive flip almost never fired in practice anyway. In the
        // rare degenerate case (fully-occluded backfacing pixel where bent
        // falls back to N in ao.glsl), the smooth two-sided blend below
        // handles whatever direction we end up with.
        //
        // Blend bent toward `n_geom` by the strength dial. The slice-based
        // GTAO bent normal estimate has a known bias toward V (per-slice
        // averaging shares V across slices) and visible direction noise on
        // uniform surfaces — blending with N at strength < 1 trades some
        // of the "cavities feel grounded" effect for stability. Done in
        // unflipped space so the mix is itself continuous across the
        // silhouette.
        float bent_t = clamp(ao_history_params.w, 0.0, 1.0);
        ibl_lookup_dir = normalize(mix(n_geom, bent_raw, bent_t));
    }

    vec3 R = reflect(-V, n_shade);
    float max_lod = mode_flags.y;
    // Smooth two-sided IBL diffuse: sample the irradiance cubemap on both
    // sides of the lookup direction and weight by the facing dot via
    // smoothstep. Pixels well inside the visible hemisphere (face_dot >
    // 0.2) collapse to ~100% irr_pos; pixels at the silhouette get a
    // 50/50 blend; backfacing pixels (face_dot < -0.2) collapse to ~100%
    // irr_neg. Cost: one extra cubemap tap per pixel, which is essentially
    // free on the tiny irradiance cubemaps (32² per face typical). Fixes
    // the "AO color of a surface abruptly changes" artifact that the prior
    // hard `if (dot(n, V) < 0) n = -n` flip produced at silhouettes.
    vec3 irr_pos =
        textureLod(samplerCube(tex_irradiance, smp_cube), ibl_lookup_dir, 0.0).rgb;
    vec3 irr_neg =
        textureLod(samplerCube(tex_irradiance, smp_cube), -ibl_lookup_dir, 0.0).rgb;
    float facing = smoothstep(-0.2, 0.2, face_dot);
    vec3 irradiance = mix(irr_neg, irr_pos, facing);

    vec3 prefiltered =
        textureLod(samplerCube(tex_prefilter, smp_cube), R, roughness * max_lod).rgb;
    vec2 brdf = textureLod(sampler2D(tex_brdf_lut, smp_lut), vec2(NdotV, roughness), 0.0).rg;

    vec3 ao_mb = gtaoMultiBounce(ao, albedo);
    float so = specularOcclusion(NdotV, ao);

    vec3 ambient = irradiance * kd * albedo * ao_mb +
                   prefiltered * (F0 * brdf.x + vec3(brdf.y)) * so;

    frag_color = vec4(ambient + Lo + emissive.xyz, base_color.a);
}
@end

@program scene scene_vs scene_fs
