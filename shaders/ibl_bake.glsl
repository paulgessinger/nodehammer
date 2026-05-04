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
    vec4 zenith_color;       // rgb (gradient model)
    vec4 horizon_color;      // rgb (gradient model)
    vec4 ground_color;       // rgb (gradient model)
    vec4 sun_dir;            // xyz=toward-sun direction, w=sun_intensity
    vec4 sun_color;          // rgb=color, w=sharpness (gradient model)
    vec4 ground_albedo;      // rgb=planet surface reflectance (Nishita)
    vec4 sky_params;         // x=turbidity, y=sky_model (0=Gradient, 1=Nishita)
};
@end

@block ibl_helpers
const float PI = 3.14159265358979323846;

// ── Gradient sky (legacy) ──────────────────────────────────────────────────
vec3 skyGradient(vec3 dir) {
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 base;
    if (dir.y >= 0.0) {
        base = mix(horizon_color.rgb, zenith_color.rgb, t * t * (3.0 - 2.0 * t));
    } else {
        float td = clamp(-dir.y, 0.0, 1.0);
        base = mix(horizon_color.rgb, ground_color.rgb, td);
    }
    float s = pow(max(dot(dir, normalize(sun_dir.xyz)), 0.0), sun_color.w);
    return base + s * sun_color.rgb * sun_dir.w;
}

// ── Nishita single-scattering sky ──────────────────────────────────────────
// Standard Earth-atmosphere model. Coordinates are in metres; the camera
// sits at sea level (planet centre below). Origin is the planet centre.
const float kPlanetRadius = 6360e3;
const float kAtmosRadius  = 6420e3;
const float kRayleighH    = 8000.0;   // Rayleigh scale height (m)
const float kMieH         = 1200.0;   // Mie scale height (m)
const float kSunAngularR  = 0.004675; // ~0.5° solar radius in radians
const float kSunCosThresh = 0.99998906; // cos(kSunAngularR)
const vec3  kBetaR        = vec3(5.802e-6, 13.558e-6, 33.1e-6); // Rayleigh extinction (1/m)
const float kBetaM        = 3.996e-6;  // Mie extinction at turbidity 1 (1/m)
const float kMieG         = 0.76;
const float kSunIrradiance = 22.0;     // arbitrary scalar — combines with sun_dir.w

// Ray-sphere intersection with a sphere centred at origin. Returns t for the
// far hit (t1) and the near hit (t0); caller checks t1 > 0 for a valid hit.
// Returns vec2(-1) when the ray misses.
vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0) return vec2(-1.0);
    float sd = sqrt(d);
    return vec2(-b - sd, -b + sd);
}

// Optical depth along a ray segment of length `t` from `ro` in direction
// `rd`. Returns vec2(rayleigh, mie) — both scalar densities, multiplied by
// extinction coefficients at the call site.
vec2 opticalDepth(vec3 ro, vec3 rd, float t, int steps) {
    float seg = t / float(steps);
    vec2 acc = vec2(0.0);
    for (int i = 0; i < steps; ++i) {
        vec3 p = ro + rd * (seg * (float(i) + 0.5));
        float h = length(p) - kPlanetRadius;
        // Density falls off exponentially with altitude. Negative h (under
        // the planet surface) shouldn't happen for valid rays but clamp to
        // avoid blow-up.
        float dr = exp(-max(h, 0.0) / kRayleighH);
        float dm = exp(-max(h, 0.0) / kMieH);
        acc += vec2(dr, dm) * seg;
    }
    return acc;
}

vec3 skyNishita(vec3 dir) {
    vec3 to_sun = normalize(sun_dir.xyz);
    // Camera is just above sea level so we always start inside the atmosphere.
    vec3 ro = vec3(0.0, kPlanetRadius + 1.0, 0.0);

    // Find where the view ray exits the atmosphere (or hits the planet).
    vec2 t_atmos = raySphere(ro, dir, kAtmosRadius);
    if (t_atmos.y < 0.0) {
        // Ray escapes downward without hitting atmosphere — shouldn't happen
        // from inside the atmosphere shell, but bail safely.
        return vec3(0.0);
    }
    float t_far = t_atmos.y;
    vec2 t_planet = raySphere(ro, dir, kPlanetRadius);
    bool hit_planet = (t_planet.x > 0.0);
    if (hit_planet) {
        t_far = min(t_far, t_planet.x);
    }

    const int kPrimarySteps = 16;
    const int kLightSteps   = 8;
    float seg = t_far / float(kPrimarySteps);

    vec3 sum_r = vec3(0.0);
    vec3 sum_m = vec3(0.0);
    vec2 cum_od = vec2(0.0); // cumulative optical depth along the primary ray

    float mu = dot(dir, to_sun);
    // Phase functions
    float phase_r = (3.0 / (16.0 * PI)) * (1.0 + mu * mu);
    float g = kMieG;
    float phase_m = (3.0 / (8.0 * PI))
        * ((1.0 - g * g) * (1.0 + mu * mu))
        / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));

    float turbidity = max(sky_params.x, 1.0);
    float beta_m = kBetaM * turbidity;

    for (int i = 0; i < kPrimarySteps; ++i) {
        vec3 p = ro + dir * (seg * (float(i) + 0.5));
        float h = length(p) - kPlanetRadius;
        float dr = exp(-max(h, 0.0) / kRayleighH) * seg;
        float dm = exp(-max(h, 0.0) / kMieH) * seg;
        cum_od += vec2(dr, dm);

        // Optical depth from sample point toward the sun. If the secondary
        // ray hits the planet the sample is in shadow → skip its contribution.
        vec2 t_sun_atmos = raySphere(p, to_sun, kAtmosRadius);
        vec2 t_sun_planet = raySphere(p, to_sun, kPlanetRadius);
        bool sun_blocked = (t_sun_planet.x > 0.0);
        if (!sun_blocked && t_sun_atmos.y > 0.0) {
            vec2 od_sun = opticalDepth(p, to_sun, t_sun_atmos.y, kLightSteps);
            vec3 tau = kBetaR * (cum_od.x + od_sun.x)
                     + vec3(beta_m * 1.1) * (cum_od.y + od_sun.y);
            vec3 transmittance = exp(-tau);
            sum_r += dr * transmittance;
            sum_m += dm * transmittance;
        }
    }

    vec3 colour = (sum_r * kBetaR * phase_r + sum_m * vec3(beta_m) * phase_m) * kSunIrradiance;

    // Ground bounce: if we hit the planet, the view ray ends there. Approximate
    // the ground radiance as Lambertian albedo * direct sun illuminance,
    // attenuated by the atmosphere transmittance from camera→ground.
    if (hit_planet) {
        vec3 g_pos = ro + dir * t_far;
        vec2 t_sun_atmos = raySphere(g_pos, to_sun, kAtmosRadius);
        vec2 t_sun_planet = raySphere(g_pos, to_sun, kPlanetRadius);
        float ndotl = max(to_sun.y, 0.0); // crude — planet normal at ground point ≈ +Y near origin
        if (t_sun_planet.x < 0.0 && t_sun_atmos.y > 0.0 && ndotl > 0.0) {
            vec2 od_sun = opticalDepth(g_pos, to_sun, t_sun_atmos.y, kLightSteps);
            vec3 tau_sun = kBetaR * od_sun.x + vec3(beta_m * 1.1) * od_sun.y;
            vec3 tau_view = kBetaR * cum_od.x + vec3(beta_m * 1.1) * cum_od.y;
            vec3 transmittance = exp(-(tau_sun + tau_view));
            colour += (ground_albedo.rgb / PI) * ndotl * kSunIrradiance * transmittance;
        }
    }

    // Direct sun disc — only when the view ray points within the solar angular
    // radius AND is not blocked by the planet. Multiply by transmittance from
    // camera through the atmosphere along the view direction (which equals the
    // total cum_od integrated above).
    if (!hit_planet && mu > kSunCosThresh) {
        vec3 tau = kBetaR * cum_od.x + vec3(beta_m * 1.1) * cum_od.y;
        vec3 transmittance = exp(-tau);
        // Solar irradiance peak — large multiplier produces a true HDR disc
        // that survives the prefilter mips and feeds tonemap roll-off.
        const float kSunDiscPeak = 1500.0;
        colour += transmittance * sun_color.rgb * kSunDiscPeak;
    }

    return max(colour, vec3(0.0)) * sun_dir.w;
}

vec3 sky(vec3 dir_in) {
    vec3 dir = normalize(dir_in);
    if (sky_params.y > 0.5) {
        return skyNishita(dir);
    }
    return skyGradient(dir);
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
    // No upper clamp: with an RGBA16F bake target the irradiance can encode
    // values > 1.0 driven by a bright sun_color, which is the HDR lever
    // tonemap responds to. Lower clamp keeps zero-dot-product cases sane.
    frag_color = vec4(max(acc, vec3(0.0)), 1.0);
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
    // No upper clamp — see irradiance_fs. Prefiltered specular benefits
    // most from HDR: the sun-disc highlights survive into the tonemap.
    frag_color = vec4(max(colour, vec3(0.0)), 1.0);
}
@end

@program ibl_pre fullscreen_vs prefilter_fs
