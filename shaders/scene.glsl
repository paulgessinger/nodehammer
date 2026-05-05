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

out vec3 v_normal_world;
out vec3 v_world_pos;

void main() {
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
    vec4 cut_params;   // x = enabled, y = large cut flag, z/w = unused
    vec4 cut_start;    // xy = unit vector at start phi
    vec4 cut_end;      // xy = unit vector at end phi
    vec4 material_mr;  // x = metallic, y = roughness, z/w = unused
    vec4 mode_flags;   // x = pbr_enable, y = prefilter_max_lod, z/w = unused
    vec4 camera_pos;   // xyz = world-space camera position
    vec4 emissive;     // xyz = emissive factor (linear, can be > 1.0), w = unused
    vec4 alpha_params; // x = alpha_mode (0 = OPAQUE, 1 = MASK), y = alpha_cutoff, z/w = unused
};

layout(binding=0) uniform textureCube tex_irradiance;
layout(binding=1) uniform textureCube tex_prefilter;
layout(binding=2) uniform texture2D   tex_brdf_lut;
layout(binding=0) uniform sampler     smp_cube;
layout(binding=1) uniform sampler     smp_lut;

in vec3 v_normal_world;
in vec3 v_world_pos;
out vec4 frag_color;

const float PI = 3.14159265358979323846;

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

    vec3 n = normalize(v_normal_world);

    if (mode_flags.x < 0.5) {
        // Lambert (legacy fast path) — preserved byte-for-byte.
        // Two-sided shading: detector inner/outer faces light regardless of winding.
        float ndl = max(abs(dot(n, -light_dir.xyz)), 0.0);
        vec3 rgb = base_color.rgb * (0.20 + 0.80 * ndl) + emissive.xyz;
        frag_color = vec4(rgb, base_color.a);
        return;
    }

    // ---- PBR (glTF 2.0 metallic-roughness) ----
    vec3 V = normalize(camera_pos.xyz - v_world_pos);
    // Two-sided: flip the normal if it faces away from the viewer. Mirrors
    // the abs() in the Lambert branch so inner detector faces still light.
    if (dot(n, V) < 0.0) {
        n = -n;
    }
    vec3 L = normalize(-light_dir.xyz);
    vec3 H = normalize(V + L);

    float NdotV = max(dot(n, V), 1e-4);
    float NdotL = max(dot(n, L), 0.0);
    float NdotH = max(dot(n, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float metallic  = clamp(material_mr.x, 0.0, 1.0);
    float roughness = clamp(material_mr.y, 0.04, 1.0);
    float a = roughness * roughness;
    float a2 = a * a;

    vec3 F0 = mix(vec3(0.04), base_color.rgb, metallic);

    vec3 F  = F_Schlick(VdotH, F0);
    float D = D_GGX(NdotH, a2);
    float G = G_SmithSchlickGGX(NdotV, NdotL, roughness);

    vec3 specular = (D * G) * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kd = (vec3(1.0) - F) * (1.0 - metallic);
    vec3 diffuse = kd * base_color.rgb / PI;

    vec3 Lo = (diffuse + specular) * NdotL * light_dir.w;

    // Image-based lighting (split-sum approximation) — always on in PBR mode.
    vec3 R = reflect(-V, n);
    float max_lod = mode_flags.y;
    vec3 irradiance = textureLod(samplerCube(tex_irradiance, smp_cube), n, 0.0).rgb;
    vec3 prefiltered =
        textureLod(samplerCube(tex_prefilter, smp_cube), R, roughness * max_lod).rgb;
    vec2 brdf = textureLod(sampler2D(tex_brdf_lut, smp_lut), vec2(NdotV, roughness), 0.0).rg;
    vec3 ambient = irradiance * kd * base_color.rgb +
                   prefiltered * (F0 * brdf.x + vec3(brdf.y));

    frag_color = vec4(ambient + Lo + emissive.xyz, base_color.a);
}
@end

@program scene scene_vs scene_fs
