#pragma once

#include <detail/json_value.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace nodehammer::viewer {

// Computed once at upload. Unlike a bounding sphere, the radial size does not
// grow with wire length; the endpoints give the closest visible depth.
struct WireLodPolicy {
    std::array<float, 3> factors{0.25f, 0.5f, 1.f};
    std::array<float, 4> fractions{1.f, 0.3f, 0.1f, 0.005f};
    float hintPixels{0.5f};
};
struct WireLodBounds {
    glm::vec3 start{}, end{};
    float radius{}, minPixels{}, maxDistance{};
    bool hint{false};
    WireLodPolicy policy{};
};

inline WireLodBounds wireLodBounds(const detail::JsonValue &extras, const glm::mat4 &world) {
    const auto *metadata = extras.find("nodehammer_wire_lod");
    if (!metadata)
        return {};
    auto number = [&](const char *key) {
        const auto *value = metadata->find(key);
        if (!value)
            return 0.f;
        if (const auto *d = value->asDouble())
            return static_cast<float>(*d);
        if (const auto *i = value->asInt())
            return static_cast<float>(*i);
        return 0.f;
    };
    const float radius = number("radius"), halfLength = number("half_length"),
                pixels = number("min_pixels"), distance = number("max_distance");
    if (!(radius > 0 && halfLength > 0 && (pixels > 0 || distance > 0)) || !std::isfinite(radius) ||
        !std::isfinite(halfLength) || !std::isfinite(pixels) || !std::isfinite(distance))
        return {};
    // Largest singular value of the radial 3x2 basis, also safe for shear and
    // nonuniform scale. Axial scaling must not inflate the radial size.
    const glm::vec3 x{world[0]}, y{world[1]};
    const float a = glm::dot(x, x), b = glm::dot(x, y), c = glm::dot(y, y);
    const float scale = std::sqrt(0.5f * (a + c + std::sqrt((a - c) * (a - c) + 4 * b * b)));
    WireLodBounds bounds{glm::vec3{world * glm::vec4{0, 0, -halfLength, 1}},
                         glm::vec3{world * glm::vec4{0, 0, halfLength, 1}}, radius * scale, pixels,
                         distance};
    if (!std::isfinite(bounds.radius))
        return {};
    auto readArray = [&](const char *key, auto &out, bool increasing) {
        const auto *value = metadata->find(key);
        if (!value)
            return;
        const auto *array = value->asArray();
        if (!array || array->size() != out.size())
            return;
        auto parsed = out;
        float previous = increasing ? 0.f : 1.f;
        for (size_t i = 0; i < parsed.size(); ++i) {
            const auto &v = (*array)[i];
            float parsedValue = v.asDouble() ? static_cast<float>(*v.asDouble())
                                : v.asInt()  ? static_cast<float>(*v.asInt())
                                             : NAN;
            if (!std::isfinite(parsedValue) ||
                (increasing ? parsedValue <= previous : parsedValue < 0 || parsedValue > previous))
                return;
            parsed[i] = parsedValue;
            previous = parsedValue;
        }
        out = parsed;
    };
    readArray("distance_factors", bounds.policy.factors, true);
    readArray("fractions", bounds.policy.fractions, false);
    if (metadata->find("hint_pixels")) {
        const float width = number("hint_pixels");
        if (std::isfinite(width) && width >= 0)
            bounds.policy.hintPixels = width;
    }
    return bounds;
}

// Spatial leaves are built once, never sorted or allocated while rendering.
// Hash ordering makes distance samples nested and stable during camera motion.
struct WireLodBlock {
    size_t begin{}, end{};
    glm::vec3 min{}, max{};
    float radius{}, minPixels{}, maxDistance{};
    bool hint{false};
    WireLodPolicy policy{};
};
struct WireLodIndex {
    std::vector<size_t> order;
    std::vector<float> ranks;
    std::vector<WireLodBlock> blocks;
};
inline float wireSampleRank(const WireLodBounds &wire) {
    uint32_t h = 2166136261u;
    for (const auto point : {wire.start, wire.end})
        for (int axis = 0; axis < 3; ++axis) {
            h ^= std::bit_cast<uint32_t>(point[axis]);
            h *= 16777619u;
        }
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) / 16777216.f;
}
// Keep a sparse structural hint at long range. Only this stable subset uses
// a minimum display width; the rest retain the requested detailed-wire LOD.
inline constexpr float kWireHintFraction = 0.005f;
inline WireLodBounds wireHintBounds(WireLodBounds wire) {
    if (wire.maxDistance > 0 && wireSampleRank(wire) < wire.policy.fractions.back()) {
        wire.minPixels = 0;
        wire.hint = true;
    }
    return wire;
}
inline float wireHintScale(const WireLodBounds &wire, const glm::mat4 &view, float focalPixels,
                           bool perspective, float nearPlane) {
    if (!wire.hint || !(wire.radius > 0 && focalPixels > 0))
        return 1;
    float depth = 1;
    if (perspective) {
        depth = std::min(-(view * glm::vec4{wire.start, 1}).z, -(view * glm::vec4{wire.end, 1}).z) -
                wire.radius;
        if (depth <= nearPlane)
            return 1;
    }
    return std::max(1.f, wire.policy.hintPixels * depth / (wire.radius * focalPixels));
}
inline WireLodIndex buildWireLodIndex(const std::vector<WireLodBounds> &wires) {
    WireLodIndex result;
    if (wires.empty() || std::any_of(wires.begin(), wires.end(), [](const auto &w) {
            return !(w.radius > 0 && w.maxDistance > 0);
        }))
        return result;
    result.order.resize(wires.size());
    std::iota(result.order.begin(), result.order.end(), size_t{0});
    result.ranks.reserve(wires.size());
    for (const auto &wire : wires)
        result.ranks.push_back(wireSampleRank(wire));
    auto split = [&](auto &&self, size_t begin, size_t end) -> void {
        WireLodBlock block{begin, end, glm::vec3{INFINITY}, glm::vec3{-INFINITY}, 0, INFINITY, 0};
        block.policy.factors.fill(0);
        block.policy.fractions.fill(0);
        glm::vec3 centerMin{INFINITY}, centerMax{-INFINITY};
        for (size_t n = begin; n < end; ++n) {
            const auto &w = wires[result.order[n]];
            const auto center = (w.start + w.end) * 0.5f;
            centerMin = glm::min(centerMin, center);
            centerMax = glm::max(centerMax, center);
            block.min = glm::min(block.min, glm::min(w.start, w.end) - w.radius);
            block.max = glm::max(block.max, glm::max(w.start, w.end) + w.radius);
            block.radius = std::max(block.radius, w.radius);
            block.minPixels = std::min(block.minPixels, w.minPixels);
            block.maxDistance = 1;
            for (size_t i = 0; i < 3; ++i)
                block.policy.factors[i] =
                    std::max(block.policy.factors[i], w.maxDistance * w.policy.factors[i]);
            for (size_t i = 0; i < 4; ++i)
                block.policy.fractions[i] =
                    std::max(block.policy.fractions[i], w.policy.fractions[i]);
        }
        if (end - begin <= 64) {
            std::sort(result.order.begin() + static_cast<std::ptrdiff_t>(begin),
                      result.order.begin() + static_cast<std::ptrdiff_t>(end),
                      [&](size_t a, size_t b) { return result.ranks[a] < result.ranks[b]; });
            result.blocks.push_back(block);
            return;
        }
        const auto extent = centerMax - centerMin;
        int axis = extent.y > extent.x ? 1 : 0;
        if (extent.z > extent[axis])
            axis = 2;
        const size_t mid = begin + (end - begin) / 2;
        std::nth_element(result.order.begin() + static_cast<std::ptrdiff_t>(begin),
                         result.order.begin() + static_cast<std::ptrdiff_t>(mid),
                         result.order.begin() + static_cast<std::ptrdiff_t>(end),
                         [&](size_t a, size_t b) {
                             return wires[a].start[axis] + wires[a].end[axis] <
                                    wires[b].start[axis] + wires[b].end[axis];
                         });
        self(self, begin, mid);
        self(self, mid, end);
    };
    split(split, 0, wires.size());
    return result;
}
inline float wireBlockDistance(const WireLodBlock &block, const glm::vec3 &eye) {
    const auto delta = glm::max(glm::max(block.min - eye, eye - block.max), glm::vec3{0});
    return glm::length(delta);
}
inline float wireDistanceFraction(float distance, float limit, const WireLodPolicy &policy = {}) {
    for (size_t i = 0; i < 3; ++i)
        if (distance < limit * policy.factors[i])
            return policy.fractions[i];
    return policy.fractions.back();
}
inline float wireDistanceFade(float rank, float distance, float limit,
                              const WireLodPolicy &policy = {}) {
    if (limit <= 0)
        return 0;
    return rank < wireDistanceFraction(distance, limit, policy) ? 0.f : 1.f;
}
inline float wirePointDistance(const WireLodBounds &wire, const glm::vec3 &eye) {
    const auto axis = wire.end - wire.start;
    const float length2 = glm::dot(axis, axis);
    const float t =
        length2 > 0 ? glm::clamp(glm::dot(eye - wire.start, axis) / length2, 0.f, 1.f) : 0;
    return std::max(0.f, glm::length(eye - (wire.start + t * axis)) - wire.radius);
}
inline bool wireBlockBelowPixels(const WireLodBlock &block, const glm::mat4 &view,
                                 float focalPixels, bool perspective, float nearPlane) {
    if (!(block.minPixels > 0))
        return false;
    float diameter = block.radius * focalPixels;
    if (perspective) {
        const auto center = (block.min + block.max) * 0.5f;
        const auto half = (block.max - block.min) * 0.5f;
        const float depth = -(view * glm::vec4{center, 1}).z -
                            glm::dot(glm::abs(glm::vec3{view[0][2], view[1][2], view[2][2]}), half);
        if (depth <= nearPlane)
            return false;
        diameter /= depth;
    }
    return diameter < block.minPixels;
}

// 0 = fully visible, 1 = omitted. In between, the shader uses screen-door
// fading. No camera-dependent tessellation or allocation is needed.
inline float wireLodFade(const WireLodBounds &wire, const glm::mat4 &view,
                         const glm::mat4 &projection, float width, float height, bool perspective,
                         float nearPlane) {
    if (!(wire.minPixels > 0 && wire.radius > 0 && width > 0 && height > 0))
        return 0;
    float diameterPixels = wire.radius * std::max(std::abs(projection[0][0]) * width,
                                                  std::abs(projection[1][1]) * height);
    if (perspective) {
        const float z0 = -(view * glm::vec4{wire.start, 1}).z;
        const float z1 = -(view * glm::vec4{wire.end, 1}).z;
        // Preserve near-plane intersections. Measuring only the midpoint would
        // hide long wires that pass close to the camera.
        const float depth = std::min(z0, z1) - wire.radius;
        if (depth <= nearPlane)
            return 0;
        diameterPixels /= depth;
    }
    if (!std::isfinite(diameterPixels))
        return 0;
    return 1.f - glm::smoothstep(wire.minPixels, 2.f * wire.minPixels, diameterPixels);
}

} // namespace nodehammer::viewer
