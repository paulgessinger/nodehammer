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
    //     -- see the screen-door discard in main(),
    // w = material-stack prefilter band width, in feature-size widths (see
    //     stack_prefilter below) -- RenderQualitySettings::material_prefilter_band.
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
    // AO is applied current-frame in the composite pass (see composite.glsl),
    // not inside the scene shader — a forward renderer can't sample this
    // frame's AO before it shades, and the frame-late variant this shader used
    // to run dragged occlusion behind the geometry during camera motion.
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
layout(binding=0) uniform sampler     smp_cube;
layout(binding=1) uniform sampler     smp_lut;

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
        // >1 guard: smoothstep(lo, hi, x) is undefined for hi <= lo.
        float band_ratio = max(1.001, cut_params.w);
        prefilter_t = smoothstep(stack_prefilter.w, band_ratio * stack_prefilter.w, footprint);
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
    // Ambient occlusion is applied later, in the composite pass, as a
    // current-frame scalar multiply on the lit color; the scene shader no
    // longer samples AO (nor the GTAO bent normal), so the irradiance lookup
    // direction is just the geometric normal. `n_geom` is unflipped (may point
    // away from V on backfacing pixels of two-sided geometry); the smooth
    // two-sided IBL blend below absorbs whatever sign it has.
    vec3 ibl_lookup_dir = n_geom;

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

    vec3 ambient = irradiance * kd * albedo +
                   prefiltered * (F0 * brdf.x + vec3(brdf.y));

    frag_color = vec4(ambient + Lo + emissive.xyz, base_color.a);
}
@end

@program scene scene_vs scene_fs
