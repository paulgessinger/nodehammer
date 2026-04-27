#include "ibl.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <sokol_time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace nodehammer::viewer {

namespace {

constexpr int kIrradianceSize = IblBakeData::kIrradianceSize;
constexpr int kPrefilterSize = IblBakeData::kPrefilterSize;
constexpr int kPrefilterMips = IblBakeData::kPrefilterMips;
constexpr int kBrdfLutSize = IblBakeData::kBrdfLutSize;
constexpr int kIrradianceSamples = 1024;
constexpr int kPrefilterSamples = 256;
constexpr int kBrdfSamples = 1024;

constexpr glm::vec3 kZenithColor{0.55f, 0.65f, 0.85f};  // soft daylight blue
constexpr glm::vec3 kHorizonColor{0.85f, 0.80f, 0.72f}; // warm haze
constexpr glm::vec3 kGroundColor{0.20f, 0.18f, 0.16f};  // dim ground
constexpr glm::vec3 kSunDir{0.4f, 0.7f, 0.6f};          // matches scene_renderer light
constexpr glm::vec3 kSunColor{1.5f, 1.4f, 1.2f};
constexpr float kSunSharpness = 64.f;

/// Procedural environment radiance for a unit direction.
glm::vec3 sky(const glm::vec3 &dir_in) {
    const glm::vec3 dir = glm::normalize(dir_in);
    const float t = glm::clamp(dir.y * 0.5f + 0.5f, 0.f, 1.f);
    glm::vec3 base;
    if (dir.y >= 0.f) {
        base = glm::mix(kHorizonColor, kZenithColor, t * t * (3.f - 2.f * t));
    } else {
        // Smoothly interpolate from horizon down into the ground.
        const float td = glm::clamp(-dir.y, 0.f, 1.f);
        base = glm::mix(kHorizonColor, kGroundColor, td);
    }
    const float sun =
        std::pow(std::max(glm::dot(dir, glm::normalize(kSunDir)), 0.f), kSunSharpness);
    return base + sun * kSunColor;
}

/// Direction (un-normalized) for face/uv on a standard sokol cubemap.
/// Face order matches sokol_gfx's documented cubemap layout:
/// 0=+X, 1=-X, 2=+Y, 3=-Y, 4=+Z, 5=-Z.
glm::vec3 cube_dir(int face, float u, float v) {
    // u, v ∈ [-1, 1]
    switch (face) {
    case 0:
        return {1.f, -v, -u}; // +X
    case 1:
        return {-1.f, -v, u}; // -X
    case 2:
        return {u, 1.f, v}; // +Y
    case 3:
        return {u, -1.f, -v}; // -Y
    case 4:
        return {u, -v, 1.f}; // +Z
    case 5:
        return {-u, -v, -1.f}; // -Z
    }
    return {0, 0, 1};
}

/// Hammersley low-discrepancy 2D sequence.
glm::vec2 hammersley(uint32_t i, uint32_t n) {
    uint32_t bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    const float radical_inverse = static_cast<float>(bits) * 2.3283064365386963e-10f;
    return {static_cast<float>(i) / static_cast<float>(n), radical_inverse};
}

/// Sample a half-vector from the GGX distribution using importance sampling.
glm::vec3 importance_sample_ggx(const glm::vec2 &xi, const glm::vec3 &n, float roughness) {
    const float a = roughness * roughness;
    const float phi = 2.f * glm::pi<float>() * xi.x;
    const float cos_theta = std::sqrt((1.f - xi.y) / (1.f + (a * a - 1.f) * xi.y));
    const float sin_theta = std::sqrt(std::max(0.f, 1.f - cos_theta * cos_theta));
    const glm::vec3 h_tangent{std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta};

    const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3{0, 0, 1} : glm::vec3{1, 0, 0};
    const glm::vec3 tangent = glm::normalize(glm::cross(up, n));
    const glm::vec3 bitangent = glm::cross(n, tangent);
    return glm::normalize(tangent * h_tangent.x + bitangent * h_tangent.y + n * h_tangent.z);
}

std::byte to_u8(float x) {
    const float c = glm::clamp(x, 0.f, 1.f);
    return static_cast<std::byte>(std::lround(c * 255.f));
}

void encode_rgba8(std::byte *dst, const glm::vec3 &rgb) {
    dst[0] = to_u8(rgb.r);
    dst[1] = to_u8(rgb.g);
    dst[2] = to_u8(rgb.b);
    dst[3] = std::byte{255};
}

float geometry_smith_ibl(float ndotv, float ndotl, float roughness) {
    const float k = (roughness * roughness) / 2.f;
    const float gv = ndotv / (ndotv * (1.f - k) + k);
    const float gl = ndotl / (ndotl * (1.f - k) + k);
    return gv * gl;
}

// ── Per-pixel bake routines ──────────────────────────────────────────────────
// Each writes 4 RGBA8 bytes at `dst`. They are the atomic unit of work used
// by both the synchronous `bake_ibl()` and the chunked `IblBakeJob` driver.

void bake_brdf_pixel(int x, int y, std::byte *dst) {
    const float roughness = (static_cast<float>(y) + 0.5f) / kBrdfLutSize;
    const float ndotv = (static_cast<float>(x) + 0.5f) / kBrdfLutSize;
    const glm::vec3 v{std::sqrt(1.f - ndotv * ndotv), 0.f, ndotv};
    const glm::vec3 n{0.f, 0.f, 1.f};

    float scale = 0.f;
    float bias = 0.f;
    for (int i = 0; i < kBrdfSamples; ++i) {
        const glm::vec2 xi =
            hammersley(static_cast<uint32_t>(i), static_cast<uint32_t>(kBrdfSamples));
        const glm::vec3 h = importance_sample_ggx(xi, n, roughness);
        const glm::vec3 l = glm::normalize(2.f * glm::dot(v, h) * h - v);

        const float ndotl = std::max(l.z, 0.f);
        const float ndoth = std::max(h.z, 0.f);
        const float vdoth = std::max(glm::dot(v, h), 0.f);

        if (ndotl > 0.f) {
            const float g = geometry_smith_ibl(ndotv, ndotl, roughness);
            const float g_vis = (g * vdoth) / std::max(ndoth * ndotv, 1e-6f);
            const float fc = std::pow(1.f - vdoth, 5.f);
            scale += (1.f - fc) * g_vis;
            bias += fc * g_vis;
        }
    }
    scale /= static_cast<float>(kBrdfSamples);
    bias /= static_cast<float>(kBrdfSamples);
    dst[0] = to_u8(scale);
    dst[1] = to_u8(bias);
    dst[2] = std::byte{0};
    dst[3] = std::byte{255};
}

void bake_irradiance_pixel(int face, int x, int y, std::byte *dst) {
    const float u = (static_cast<float>(x) + 0.5f) / kIrradianceSize * 2.f - 1.f;
    const float v = (static_cast<float>(y) + 0.5f) / kIrradianceSize * 2.f - 1.f;
    const glm::vec3 n = glm::normalize(cube_dir(face, u, v));

    // Cosine-weighted hemisphere sampling around n.
    const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3{0, 0, 1} : glm::vec3{1, 0, 0};
    const glm::vec3 tangent = glm::normalize(glm::cross(up, n));
    const glm::vec3 bitangent = glm::cross(n, tangent);

    glm::vec3 acc{0.f};
    for (int i = 0; i < kIrradianceSamples; ++i) {
        const glm::vec2 xi =
            hammersley(static_cast<uint32_t>(i), static_cast<uint32_t>(kIrradianceSamples));
        const float phi = 2.f * glm::pi<float>() * xi.x;
        const float cos_theta = std::sqrt(1.f - xi.y);
        const float sin_theta = std::sqrt(xi.y);
        const glm::vec3 dir_t{std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta};
        const glm::vec3 dir = glm::normalize(tangent * dir_t.x + bitangent * dir_t.y + n * dir_t.z);
        acc += sky(dir);
    }
    // Cosine-weighted Monte Carlo integral with PDF = cosθ/π already
    // produces the irradiance directly.
    acc /= static_cast<float>(kIrradianceSamples);
    encode_rgba8(dst, acc);
}

void bake_prefilter_pixel(int mip, int face, int x, int y, std::byte *dst) {
    const int size = std::max(1, kPrefilterSize >> mip);
    const float roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterMips - 1);
    const float fsize = static_cast<float>(size);
    const float u = (static_cast<float>(x) + 0.5f) / fsize * 2.f - 1.f;
    const float v = (static_cast<float>(y) + 0.5f) / fsize * 2.f - 1.f;
    const glm::vec3 r = glm::normalize(cube_dir(face, u, v));
    const glm::vec3 n = r;
    const glm::vec3 view = r;

    glm::vec3 acc{0.f};
    float weight_sum = 0.f;
    for (int i = 0; i < kPrefilterSamples; ++i) {
        const glm::vec2 xi =
            hammersley(static_cast<uint32_t>(i), static_cast<uint32_t>(kPrefilterSamples));
        const glm::vec3 h = importance_sample_ggx(xi, n, roughness);
        const glm::vec3 l = glm::normalize(2.f * glm::dot(view, h) * h - view);
        const float ndotl = std::max(glm::dot(n, l), 0.f);
        if (ndotl > 0.f) {
            acc += sky(l) * ndotl;
            weight_sum += ndotl;
        }
    }
    const glm::vec3 colour = weight_sum > 0.f ? acc / weight_sum : glm::vec3{0.f};
    encode_rgba8(dst, colour);
}

// ── Buffer sizing helpers ────────────────────────────────────────────────────

constexpr size_t brdf_lut_byte_size() {
    return static_cast<size_t>(kBrdfLutSize) * kBrdfLutSize * 4;
}

constexpr size_t irradiance_face_bytes() {
    return static_cast<size_t>(kIrradianceSize) * kIrradianceSize * 4;
}

constexpr size_t prefilter_face_bytes(int mip) {
    const int size = (kPrefilterSize >> mip) > 1 ? (kPrefilterSize >> mip) : 1;
    return static_cast<size_t>(size) * static_cast<size_t>(size) * 4;
}

void allocate_bake_buffers(IblBakeData &data) {
    data.brdf_lut.assign(brdf_lut_byte_size(), std::byte{0});
    data.irradiance_combined.assign(irradiance_face_bytes() * 6, std::byte{0});
    for (size_t m = 0; m < kPrefilterMips; ++m) {
        data.prefilter_mips[m].assign(prefilter_face_bytes(static_cast<int>(m)) * 6, std::byte{0});
    }
}

} // namespace

IblBakeData bake_ibl() {
    IblBakeData data;
    allocate_bake_buffers(data);

    // BRDF LUT
    for (int y = 0; y < kBrdfLutSize; ++y) {
        for (int x = 0; x < kBrdfLutSize; ++x) {
            const size_t idx = (static_cast<size_t>(y) * kBrdfLutSize + static_cast<size_t>(x)) * 4;
            bake_brdf_pixel(x, y, &data.brdf_lut[idx]);
        }
    }

    // Irradiance cubemap (6 faces, packed in [+X, -X, +Y, -Y, +Z, -Z] order).
    const size_t irr_face_sz = irradiance_face_bytes();
    for (int face = 0; face < 6; ++face) {
        std::byte *face_ptr = &data.irradiance_combined[static_cast<size_t>(face) * irr_face_sz];
        for (int y = 0; y < kIrradianceSize; ++y) {
            for (int x = 0; x < kIrradianceSize; ++x) {
                const size_t idx =
                    (static_cast<size_t>(y) * kIrradianceSize + static_cast<size_t>(x)) * 4;
                bake_irradiance_pixel(face, x, y, &face_ptr[idx]);
            }
        }
    }

    // Prefiltered specular cubemap (mipped).
    for (int mip = 0; mip < kPrefilterMips; ++mip) {
        const int size = std::max(1, kPrefilterSize >> mip);
        const size_t face_sz = prefilter_face_bytes(mip);
        for (int face = 0; face < 6; ++face) {
            std::byte *face_ptr =
                &data.prefilter_mips[static_cast<size_t>(mip)][static_cast<size_t>(face) * face_sz];
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(size) +
                                        static_cast<size_t>(x)) *
                                       4;
                    bake_prefilter_pixel(mip, face, x, y, &face_ptr[idx]);
                }
            }
        }
    }
    return data;
}

void IblResources::create_dummy() {
    // 1x1 placeholder textures so sg_bindings always have something to
    // sample from. The shader gates IBL contributions on mode_flags.y, so
    // these values are only visible when the user toggles PBR on before
    // the real bake completes.

    // BRDF LUT: scale=1, bias=0 (neutral, matches the Schlick-Fresnel
    // identity for a smooth surface).
    const std::array<std::byte, 4> lut_pixel{std::byte{255}, std::byte{0}, std::byte{0},
                                             std::byte{255}};
    sg_image_desc lut_desc{};
    lut_desc.type = SG_IMAGETYPE_2D;
    lut_desc.width = 1;
    lut_desc.height = 1;
    lut_desc.num_mipmaps = 1;
    lut_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    lut_desc.data.mip_levels[0].ptr = lut_pixel.data();
    lut_desc.data.mip_levels[0].size = lut_pixel.size();
    lut_desc.label = "ibl_brdf_lut_dummy";
    brdf_lut = sg_make_image(&lut_desc);

    // Irradiance cube: mid-grey sky on every face.
    std::array<std::byte, 4 * 6> irr_pixels{};
    for (size_t f = 0; f < 6; ++f) {
        irr_pixels[f * 4 + 0] = std::byte{128};
        irr_pixels[f * 4 + 1] = std::byte{128};
        irr_pixels[f * 4 + 2] = std::byte{128};
        irr_pixels[f * 4 + 3] = std::byte{255};
    }
    sg_image_desc irr_desc{};
    irr_desc.type = SG_IMAGETYPE_CUBE;
    irr_desc.width = 1;
    irr_desc.height = 1;
    irr_desc.num_mipmaps = 1;
    irr_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    irr_desc.data.mip_levels[0].ptr = irr_pixels.data();
    irr_desc.data.mip_levels[0].size = irr_pixels.size();
    irr_desc.label = "ibl_irradiance_dummy";
    irradiance = sg_make_image(&irr_desc);

    // Prefilter cube: mid-grey, single mip (max-LOD reflects this in the
    // shader since prefilter_mip_count == 1).
    sg_image_desc pre_desc{};
    pre_desc.type = SG_IMAGETYPE_CUBE;
    pre_desc.width = 1;
    pre_desc.height = 1;
    pre_desc.num_mipmaps = 1;
    pre_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    pre_desc.data.mip_levels[0].ptr = irr_pixels.data();
    pre_desc.data.mip_levels[0].size = irr_pixels.size();
    pre_desc.label = "ibl_prefilter_dummy";
    prefilter = sg_make_image(&pre_desc);
    prefilter_mip_count = 1;

    sg_view_desc irr_view{};
    irr_view.texture.image = irradiance;
    irr_view.label = "ibl_irradiance_view";
    irradiance_view = sg_make_view(&irr_view);

    sg_view_desc pre_view{};
    pre_view.texture.image = prefilter;
    pre_view.label = "ibl_prefilter_view";
    prefilter_view = sg_make_view(&pre_view);

    sg_view_desc lut_view{};
    lut_view.texture.image = brdf_lut;
    lut_view.label = "ibl_brdf_lut_view";
    brdf_lut_view = sg_make_view(&lut_view);

    sg_sampler_desc cube_smp{};
    cube_smp.min_filter = SG_FILTER_LINEAR;
    cube_smp.mag_filter = SG_FILTER_LINEAR;
    cube_smp.mipmap_filter = SG_FILTER_LINEAR;
    cube_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.label = "ibl_cube_sampler";
    cube_sampler = sg_make_sampler(&cube_smp);

    sg_sampler_desc lut_smp{};
    lut_smp.min_filter = SG_FILTER_LINEAR;
    lut_smp.mag_filter = SG_FILTER_LINEAR;
    lut_smp.mipmap_filter = SG_FILTER_NEAREST;
    lut_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.label = "ibl_lut_sampler";
    lut_sampler = sg_make_sampler(&lut_smp);
}

void IblResources::upload(const IblBakeData &data) {
    sg_image_desc lut_desc{};
    lut_desc.type = SG_IMAGETYPE_2D;
    lut_desc.width = kBrdfLutSize;
    lut_desc.height = kBrdfLutSize;
    lut_desc.num_mipmaps = 1;
    lut_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    lut_desc.data.mip_levels[0].ptr = data.brdf_lut.data();
    lut_desc.data.mip_levels[0].size = data.brdf_lut.size();
    lut_desc.label = "ibl_brdf_lut";
    brdf_lut = sg_make_image(&lut_desc);

    sg_image_desc irr_desc{};
    irr_desc.type = SG_IMAGETYPE_CUBE;
    irr_desc.width = kIrradianceSize;
    irr_desc.height = kIrradianceSize;
    irr_desc.num_mipmaps = 1;
    irr_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    irr_desc.data.mip_levels[0].ptr = data.irradiance_combined.data();
    irr_desc.data.mip_levels[0].size = data.irradiance_combined.size();
    irr_desc.label = "ibl_irradiance";
    irradiance = sg_make_image(&irr_desc);

    sg_image_desc pre_desc{};
    pre_desc.type = SG_IMAGETYPE_CUBE;
    pre_desc.width = kPrefilterSize;
    pre_desc.height = kPrefilterSize;
    pre_desc.num_mipmaps = kPrefilterMips;
    pre_desc.pixel_format = SG_PIXELFORMAT_RGBA8;
    for (size_t m = 0; m < kPrefilterMips; ++m) {
        pre_desc.data.mip_levels[m].ptr = data.prefilter_mips[m].data();
        pre_desc.data.mip_levels[m].size = data.prefilter_mips[m].size();
    }
    pre_desc.label = "ibl_prefilter";
    prefilter = sg_make_image(&pre_desc);
    prefilter_mip_count = kPrefilterMips;

    sg_view_desc irr_view{};
    irr_view.texture.image = irradiance;
    irr_view.label = "ibl_irradiance_view";
    irradiance_view = sg_make_view(&irr_view);

    sg_view_desc pre_view{};
    pre_view.texture.image = prefilter;
    pre_view.label = "ibl_prefilter_view";
    prefilter_view = sg_make_view(&pre_view);

    sg_view_desc lut_view{};
    lut_view.texture.image = brdf_lut;
    lut_view.label = "ibl_brdf_lut_view";
    brdf_lut_view = sg_make_view(&lut_view);

    sg_sampler_desc cube_smp{};
    cube_smp.min_filter = SG_FILTER_LINEAR;
    cube_smp.mag_filter = SG_FILTER_LINEAR;
    cube_smp.mipmap_filter = SG_FILTER_LINEAR;
    cube_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.wrap_w = SG_WRAP_CLAMP_TO_EDGE;
    cube_smp.label = "ibl_cube_sampler";
    cube_sampler = sg_make_sampler(&cube_smp);

    sg_sampler_desc lut_smp{};
    lut_smp.min_filter = SG_FILTER_LINEAR;
    lut_smp.mag_filter = SG_FILTER_LINEAR;
    lut_smp.mipmap_filter = SG_FILTER_NEAREST;
    lut_smp.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    lut_smp.label = "ibl_lut_sampler";
    lut_sampler = sg_make_sampler(&lut_smp);
}

void IblResources::release() {
    // Views must be destroyed before the images they reference.
    if (irradiance_view.id != SG_INVALID_ID) {
        sg_destroy_view(irradiance_view);
        irradiance_view = sg_view{};
    }
    if (prefilter_view.id != SG_INVALID_ID) {
        sg_destroy_view(prefilter_view);
        prefilter_view = sg_view{};
    }
    if (brdf_lut_view.id != SG_INVALID_ID) {
        sg_destroy_view(brdf_lut_view);
        brdf_lut_view = sg_view{};
    }
    if (irradiance.id != SG_INVALID_ID) {
        sg_destroy_image(irradiance);
        irradiance = sg_image{};
    }
    if (prefilter.id != SG_INVALID_ID) {
        sg_destroy_image(prefilter);
        prefilter = sg_image{};
    }
    if (brdf_lut.id != SG_INVALID_ID) {
        sg_destroy_image(brdf_lut);
        brdf_lut = sg_image{};
    }
    if (cube_sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(cube_sampler);
        cube_sampler = sg_sampler{};
    }
    if (lut_sampler.id != SG_INVALID_ID) {
        sg_destroy_sampler(lut_sampler);
        lut_sampler = sg_sampler{};
    }
    prefilter_mip_count = 0;
}

// ── IblBakeJob ───────────────────────────────────────────────────────────────

IblBakeJob::~IblBakeJob() {
#ifndef __EMSCRIPTEN__
    if (worker_.joinable()) {
        worker_.join();
    }
#endif
}

void IblBakeJob::start() {
    if (started_) {
        return;
    }
    started_ = true;

#ifdef __EMSCRIPTEN__
    allocate_bake_buffers(partial_);
    stage_ = Stage::BrdfLut;
    face_ = 0;
    mip_ = 0;
    y_ = 0;
    x_ = 0;
#else
    worker_ = std::thread([this] {
        result_ = bake_ibl();
        done_.store(true, std::memory_order_release);
    });
#endif
}

#ifdef __EMSCRIPTEN__

void IblBakeJob::bake_one_pixel() {
    switch (stage_) {
    case Stage::BrdfLut: {
        const size_t idx = (static_cast<size_t>(y_) * kBrdfLutSize + static_cast<size_t>(x_)) * 4;
        bake_brdf_pixel(x_, y_, &partial_.brdf_lut[idx]);
        break;
    }
    case Stage::Irradiance: {
        const size_t face_off = static_cast<size_t>(face_) * irradiance_face_bytes();
        const size_t idx =
            face_off + (static_cast<size_t>(y_) * kIrradianceSize + static_cast<size_t>(x_)) * 4;
        bake_irradiance_pixel(face_, x_, y_, &partial_.irradiance_combined[idx]);
        break;
    }
    case Stage::Prefilter: {
        const int size = std::max(1, kPrefilterSize >> mip_);
        const size_t face_off = static_cast<size_t>(face_) * prefilter_face_bytes(mip_);
        const size_t idx =
            face_off +
            (static_cast<size_t>(y_) * static_cast<size_t>(size) + static_cast<size_t>(x_)) * 4;
        bake_prefilter_pixel(mip_, face_, x_, y_,
                             &partial_.prefilter_mips[static_cast<size_t>(mip_)][idx]);
        break;
    }
    case Stage::Idle:
    case Stage::Done:
        break;
    }
}

void IblBakeJob::advance_iterator() {
    switch (stage_) {
    case Stage::BrdfLut:
        if (++x_ >= kBrdfLutSize) {
            x_ = 0;
            if (++y_ >= kBrdfLutSize) {
                y_ = 0;
                stage_ = Stage::Irradiance;
            }
        }
        break;
    case Stage::Irradiance:
        if (++x_ >= kIrradianceSize) {
            x_ = 0;
            if (++y_ >= kIrradianceSize) {
                y_ = 0;
                if (++face_ >= 6) {
                    face_ = 0;
                    stage_ = Stage::Prefilter;
                }
            }
        }
        break;
    case Stage::Prefilter: {
        const int size = std::max(1, kPrefilterSize >> mip_);
        if (++x_ >= size) {
            x_ = 0;
            if (++y_ >= size) {
                y_ = 0;
                if (++face_ >= 6) {
                    face_ = 0;
                    if (++mip_ >= kPrefilterMips) {
                        mip_ = 0;
                        stage_ = Stage::Done;
                    }
                }
            }
        }
        break;
    }
    case Stage::Idle:
    case Stage::Done:
        break;
    }
}

bool IblBakeJob::advance(uint64_t budget_ns) {
    if (!started_) {
        start();
    }
    if (stage_ == Stage::Done) {
        return true;
    }
    // Check the clock every N pixels rather than on every iteration so the
    // stm_now() / stm_diff() cost doesn't dominate cheap pixels (e.g. small
    // prefilter mips).
    constexpr int kPixelsPerClockCheck = 8;
    const uint64_t start_ticks = stm_now();
    int since_check = 0;
    while (stage_ != Stage::Done) {
        bake_one_pixel();
        advance_iterator();
        if (++since_check >= kPixelsPerClockCheck) {
            since_check = 0;
            if (stm_ns(stm_diff(stm_now(), start_ticks)) >= static_cast<double>(budget_ns)) {
                break;
            }
        }
    }
    return stage_ == Stage::Done;
}

#else

bool IblBakeJob::advance(uint64_t /*budget_ns*/) {
    if (!started_) {
        start();
    }
    if (done_.load(std::memory_order_acquire)) {
        if (worker_.joinable()) {
            worker_.join();
        }
        return true;
    }
    return false;
}

#endif

IblBakeData IblBakeJob::take() {
    taken_ = true;
#ifdef __EMSCRIPTEN__
    return std::move(partial_);
#else
    return std::move(result_);
#endif
}

} // namespace nodehammer::viewer
