// Procedural-sky IBL bake — fragment-shader path. Three @program targets,
// all sharing a fullscreen-triangle vertex shader. Output is RGBA8 LDR
// matching the runtime sampling format in scene.glsl.
//
// Compiled by sokol-shdc to every backend the viewer ships (Metal, D3D11,
// GLCore, GLES3, WGPU). No compute shaders — sokol-shdc compute requires
// GLSL 310 ES which WebGL2 does not expose.
//
// Sample counts and sky parameters are uniform-controlled (see `ibl_settings`
// block) so the viewer UI can tune them without recompile.

@module ibl_bake

@vs fullscreen_vs
in vec2 a_pos;
out vec2 v_uv;
void main() {
    v_uv = a_pos;             // [-1, 1] mapped through the FS for cube-dir reconstruction
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
@end

@block ibl_settings_block
// Identical layout in all three FSs so sokol-shdc emits one C struct.
// `face_rough_samples` packs the per-pass varying parameters; the rest are
// rebake-time scene parameters fed from the viewer UI.
layout(binding=0) uniform ibl_settings {
    vec4 face_rough_samples; // x=face index, y=roughness, z=sample count
    vec4 zenith_color;       // rgb
    vec4 horizon_color;      // rgb
    vec4 ground_color;       // rgb
    vec4 sun_dir;            // xyz=direction, w unused
    vec4 sun_color;          // rgb=color, w=sharpness
};
@end

@block ibl_helpers
const float PI = 3.14159265358979323846;

vec3 sky(vec3 dir_in) {
    vec3 dir = normalize(dir_in);
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 base;
    if (dir.y >= 0.0) {
        base = mix(horizon_color.rgb, zenith_color.rgb, t * t * (3.0 - 2.0 * t));
    } else {
        float td = clamp(-dir.y, 0.0, 1.0);
        base = mix(horizon_color.rgb, ground_color.rgb, td);
    }
    float s = pow(max(dot(dir, normalize(sun_dir.xyz)), 0.0), sun_color.w);
    return base + s * sun_color.rgb;
}

// Standard sokol cube face order: 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
vec3 cubeDir(int face, float u, float v) {
    if (face == 0) return vec3( 1.0,  -v,  -u);
    if (face == 1) return vec3(-1.0,  -v,   u);
    if (face == 2) return vec3(   u, 1.0,   v);
    if (face == 3) return vec3(   u,-1.0,  -v);
    if (face == 4) return vec3(   u,  -v, 1.0);
                   return vec3(  -u,  -v,-1.0);
}

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), radicalInverseVdC(i));
}

vec3 importanceSampleGgx(vec2 xi, vec3 n, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
    vec3 h_t = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);

    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);
    return normalize(tangent * h_t.x + bitangent * h_t.y + n * h_t.z);
}

float geometrySmithIbl(float ndotv, float ndotl, float roughness) {
    float k = (roughness * roughness) / 2.0;
    float gv = ndotv / (ndotv * (1.0 - k) + k);
    float gl = ndotl / (ndotl * (1.0 - k) + k);
    return gv * gl;
}
@end

// ── BRDF LUT ───────────────────────────────────────────────────────────────
// Sky parameters are unused here (the LUT is purely a function of roughness
// and n·v) but the uniform block is shared with the other two FSs.
@fs brdf_fs
@include_block ibl_settings_block
@include_block ibl_helpers
in vec2 v_uv;
out vec4 frag_color;

void main() {
    float ndotv     = clamp(v_uv.x * 0.5 + 0.5, 0.0, 1.0);
    float roughness = clamp(v_uv.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 v = vec3(sqrt(1.0 - ndotv * ndotv), 0.0, ndotv);
    vec3 n = vec3(0.0, 0.0, 1.0);

    uint kSamples = uint(max(face_rough_samples.z + 0.5, 1.0));
    float scale = 0.0;
    float bias  = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = hammersley(i, kSamples);
        vec3 h = importanceSampleGgx(xi, n, roughness);
        vec3 l = normalize(2.0 * dot(v, h) * h - v);
        float ndotl = max(l.z, 0.0);
        float ndoth = max(h.z, 0.0);
        float vdoth = max(dot(v, h), 0.0);
        if (ndotl > 0.0) {
            float g = geometrySmithIbl(ndotv, ndotl, roughness);
            float g_vis = (g * vdoth) / max(ndoth * ndotv, 1e-6);
            float fc = pow(1.0 - vdoth, 5.0);
            scale += (1.0 - fc) * g_vis;
            bias  += fc * g_vis;
        }
    }
    scale /= float(kSamples);
    bias  /= float(kSamples);
    frag_color = vec4(scale, bias, 0.0, 1.0);
}
@end

@program ibl_brdf fullscreen_vs brdf_fs

// ── Irradiance cubemap ─────────────────────────────────────────────────────
@fs irradiance_fs
@include_block ibl_settings_block
@include_block ibl_helpers
in vec2 v_uv;
out vec4 frag_color;

void main() {
    int face = int(face_rough_samples.x + 0.5);
    uint kSamples = uint(max(face_rough_samples.z + 0.5, 1.0));

    vec3 n = normalize(cubeDir(face, v_uv.x, v_uv.y));
    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, n));
    vec3 bitangent = cross(n, tangent);

    vec3 acc = vec3(0.0);
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = hammersley(i, kSamples);
        float phi = 2.0 * PI * xi.x;
        float cos_theta = sqrt(1.0 - xi.y);
        float sin_theta = sqrt(xi.y);
        vec3 dir_t = vec3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
        vec3 dir = normalize(tangent * dir_t.x + bitangent * dir_t.y + n * dir_t.z);
        acc += sky(dir);
    }
    acc /= float(kSamples);
    frag_color = vec4(clamp(acc, 0.0, 1.0), 1.0);
}
@end

@program ibl_irr fullscreen_vs irradiance_fs

// ── Prefiltered specular cubemap ───────────────────────────────────────────
@fs prefilter_fs
@include_block ibl_settings_block
@include_block ibl_helpers
in vec2 v_uv;
out vec4 frag_color;

void main() {
    int face = int(face_rough_samples.x + 0.5);
    float roughness = face_rough_samples.y;
    uint kSamples = uint(max(face_rough_samples.z + 0.5, 1.0));

    vec3 r = normalize(cubeDir(face, v_uv.x, v_uv.y));
    vec3 n = r;
    vec3 view = r;

    vec3 acc = vec3(0.0);
    float weight_sum = 0.0;
    for (uint i = 0u; i < kSamples; ++i) {
        vec2 xi = hammersley(i, kSamples);
        vec3 h = importanceSampleGgx(xi, n, roughness);
        vec3 l = normalize(2.0 * dot(view, h) * h - view);
        float ndotl = max(dot(n, l), 0.0);
        if (ndotl > 0.0) {
            acc += sky(l) * ndotl;
            weight_sum += ndotl;
        }
    }
    vec3 colour = weight_sum > 0.0 ? acc / weight_sum : vec3(0.0);
    frag_color = vec4(clamp(colour, 0.0, 1.0), 1.0);
}
@end

@program ibl_pre fullscreen_vs prefilter_fs
