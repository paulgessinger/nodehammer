#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <ir/semantic.hpp>
#include <stdexcept>

namespace nodehammer::tessellation {
// Linear interpolation between corresponding end-cap vertices defines a bilinear
// side P(u,t). On a z strip of width 1/n, its difference from the two linear
// triangles is D*u*(1-v)/n or D*v*(1-u)/n, with magnitude <= |D|/(4*n).
// A common strip count makes adjacent sides share exactly the same edge vertices.
inline ir::semantic::TessellatedShape
tessellateGenericTrap(const ir::semantic::GenericTrapShape &shape, double tolerance) {
    if (!std::isfinite(tolerance) || tolerance <= 0)
        throw std::invalid_argument(
            "GenericTrap tessellation tolerance must be finite and positive");
    const auto &vertices = shape.vertices;
    const double dz = shape.dz;
    if (vertices.size() != 8 || !std::isfinite(dz) || dz <= 0)
        throw std::runtime_error("invalid GeoGenericTrap dimensions");
    std::array<glm::dvec2, 4> bottom{}, top{}, delta{};
    for (size_t i = 0; i < 4; ++i) {
        bottom[i] = {vertices[i].x, vertices[i].y};
        top[i] = {vertices[i + 4].x, vertices[i + 4].y};
        delta[i] = top[i] - bottom[i];
        for (auto p : {bottom[i], top[i], delta[i]})
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                throw std::runtime_error("nonfinite GeoGenericTrap vertex");
    }
    auto cross = [](glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; };
    double area = 0;
    for (size_t i = 0; i < 4; ++i)
        area += cross(bottom[i] + delta[i] * 0.5, bottom[(i + 1) % 4] + delta[(i + 1) % 4] * 0.5);
    if (!std::isfinite(area) || area == 0)
        throw std::runtime_error("degenerate GeoGenericTrap cross section");
    const double winding = area > 0 ? 1 : -1;
    // Check convexity throughout z, not just at the caps: each consecutive-edge
    // cross product is quadratic in t. Repeated vertices (wedges/pyramids) are OK.
    for (size_t i = 0; i < 4; ++i) {
        const auto j = (i + 1) % 4, k = (i + 2) % 4;
        const auto a = bottom[j] - bottom[i], b = bottom[k] - bottom[j];
        const auto da = delta[j] - delta[i], db = delta[k] - delta[j];
        const double c0 = winding * cross(a, b);
        const double c1 = winding * (cross(da, b) + cross(a, db));
        const double c2 = winding * cross(da, db);
        auto check = [&](double t) {
            const double turn = (c2 * t + c1) * t + c0;
            if (!std::isfinite(turn) || turn < 0)
                throw std::runtime_error("nonconvex or inverted GeoGenericTrap cross section");
        };
        check(0);
        check(1);
        if (c2 > 0) {
            const double t = -c1 / (2 * c2);
            if (t > 0 && t < 1)
                check(t);
        }
    }
    // A section may collapse inside the trap without changing its winding
    // (e.g. a 180-degree twist). Reject that pinched, non-manifold solid.
    double area1 = 0, area2 = 0;
    for (size_t i = 1; i < 3; ++i) {
        const auto a = bottom[i] - bottom[0], b = bottom[i + 1] - bottom[0];
        const auto da = delta[i] - delta[0], db = delta[i + 1] - delta[0];
        area1 += winding * (cross(da, b) + cross(a, db));
        area2 += winding * cross(da, db);
    }
    if (area2 > 0) {
        const double t = -area1 / (2 * area2);
        if (t > 0 && t < 1) {
            double sectionArea = 0;
            for (size_t i = 1; i < 3; ++i)
                sectionArea +=
                    winding * cross(bottom[i] - bottom[0] + t * (delta[i] - delta[0]),
                                    bottom[i + 1] - bottom[0] + t * (delta[i + 1] - delta[0]));
            if (sectionArea <= 0)
                throw std::runtime_error("pinched GeoGenericTrap cross section");
        }
    }
    double twist = 0;
    for (size_t i = 0; i < 4; ++i)
        twist = std::max(twist, glm::length(delta[(i + 1) % 4] - delta[i]));
    const double needed = std::ceil((twist / tolerance) / 4);
    constexpr size_t maxStrips = 16384;
    if (!std::isfinite(needed) || needed > maxStrips)
        throw std::runtime_error(
            "GeoGenericTrap tessellation exceeds 16384 strips; increase tolerance");
    const auto strips = static_cast<size_t>(std::max(1.0, needed));
    ir::semantic::TessellatedShape mesh;
    mesh.triangles.reserve(8 * strips + 4);
    auto triangle = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c) {
        const auto normal = glm::cross(b - a, c - a);
        if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z))
            throw std::runtime_error("GeoGenericTrap tessellation coordinate overflow");
        if (normal == glm::dvec3{0})
            return;
        if (winding < 0)
            std::swap(b, c);
        mesh.triangles.push_back({{a, b, c}});
    };
    auto ring = [&](size_t strip) {
        std::array<glm::dvec3, 4> out{};
        const double t = static_cast<double>(strip) / static_cast<double>(strips);
        for (size_t i = 0; i < 4; ++i) {
            // Preserve cap coordinates bit-for-bit, including collapsed vertices.
            const auto xy = strip == 0        ? bottom[i]
                            : strip == strips ? top[i]
                                              : bottom[i] + t * delta[i];
            out[i] = {xy, strip == strips ? dz : -dz + 2 * dz * t};
        }
        return out;
    };
    auto lower = ring(0);
    triangle(lower[0], lower[2], lower[1]);
    triangle(lower[0], lower[3], lower[2]);
    for (size_t strip = 1; strip <= strips; ++strip) {
        auto upper = ring(strip);
        for (size_t i = 0; i < 4; ++i) {
            const auto j = (i + 1) % 4;
            triangle(lower[i], lower[j], upper[j]);
            triangle(lower[i], upper[j], upper[i]);
        }
        lower = upper;
    }
    triangle(lower[0], lower[1], lower[2]);
    triangle(lower[0], lower[2], lower[3]);
    return mesh;
}
} // namespace nodehammer::tessellation
