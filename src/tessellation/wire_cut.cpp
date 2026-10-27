#include <tessellation/wire_cut.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace nodehammer::tessellation {
namespace {
constexpr double tau = 2 * std::numbers::pi;
double normalize(double angle) {
    angle = std::fmod(angle, tau);
    return angle < 0 ? angle + tau : angle;
}
} // namespace
bool isStraightSlenderWire(const ir::semantic::ShapeVariant &shape) {
    const auto *tube = std::get_if<ir::semantic::TubeShape>(&shape);
    return tube && tube->rMin == 0 && tube->rMax > 0 && std::isfinite(tube->rMax) &&
           std::isfinite(tube->dz) && std::isfinite(tube->phiStart) &&
           tube->dz / tube->rMax >= 100 && std::abs(tube->phiDelta - tau) < 1e-9;
}
std::optional<std::vector<WireInterval>> clipWireCenterline(const ir::semantic::TubeShape &wire,
                                                            const ir::semantic::TubeShape &sector,
                                                            const glm::dmat4 &wireToSector) {
    if (!isStraightSlenderWire(wire) || sector.rMin != 0 || !std::isfinite(sector.rMax) ||
        !std::isfinite(sector.dz) || !std::isfinite(sector.phiStart) ||
        !std::isfinite(sector.phiDelta) || sector.phiDelta <= 1e-9 || sector.phiDelta >= tau - 1e-9)
        return std::nullopt;
    // Restrict this approximation to rigid frames. Scaled/sheared imported
    // tubes keep their exact existing Boolean behavior.
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            if (!std::isfinite(wireToSector[i][j]))
                return std::nullopt;
    if (wireToSector[0][3] != 0 || wireToSector[1][3] != 0 || wireToSector[2][3] != 0 ||
        std::abs(wireToSector[3][3] - 1) > 1e-10)
        return std::nullopt;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (std::abs(glm::dot(glm::dvec3(wireToSector[i]), glm::dvec3(wireToSector[j])) -
                         (i == j ? 1 : 0)) > 1e-8)
                return std::nullopt;
    const glm::dvec3 center = wireToSector[3];
    const glm::dvec3 axis = wireToSector[2];
    const auto first = center - axis * wire.dz;
    const auto last = center + axis * wire.dz;
    for (const auto &p : {first, last}) {
        if (std::hypot(p.x, p.y) + wire.rMax >= sector.rMax ||
            std::abs(p.z) + wire.rMax >= sector.dz)
            return std::nullopt; // finite cutter side/end faces would also cut
    }
    const auto transverse = glm::dvec2{axis};
    const double norm2 = glm::dot(transverse, transverse);
    const double nearest =
        norm2 > 1e-16
            ? std::clamp(-glm::dot(glm::dvec2{center}, transverse) / norm2, -wire.dz, wire.dz)
            : 0;
    if (glm::length(glm::dvec2{center + nearest * axis}) <= 4 * wire.rMax)
        return std::nullopt; // angular discontinuity near the global axis

    std::vector<double> endpoints{-wire.dz, wire.dz};
    for (const double angle : {sector.phiStart, sector.phiStart + sector.phiDelta}) {
        const glm::dvec2 n{-std::sin(angle), std::cos(angle)};
        const double f = glm::dot(n, glm::dvec2{center});
        const double slope = glm::dot(n, transverse);
        const double f0 = f - slope * wire.dz, f1 = f + slope * wire.dz;
        if (f0 * f1 > 0 && std::min(std::abs(f0), std::abs(f1)) > wire.rMax)
            continue; // cylinder entirely clear of this plane
        // Bound the axial error from replacing an oblique cap with a plane
        // perpendicular to the wire: <= 0.5% of the full original length.
        // A grazing/parallel wire outside this bound uses exact Manifold.
        if (std::abs(slope) < 1e-12 || wire.rMax / std::abs(slope) > 0.01 * wire.dz)
            return std::nullopt;
        const double crossing = -f / slope;
        if (crossing > -wire.dz && crossing < wire.dz)
            endpoints.push_back(crossing);
        else if (std::min(std::abs(f0), std::abs(f1)) <= wire.rMax)
            return std::nullopt; // endpoint grazing can leave a small cap sliver
    }
    std::sort(endpoints.begin(), endpoints.end());
    endpoints.erase(std::unique(endpoints.begin(), endpoints.end()), endpoints.end());
    std::vector<WireInterval> kept;
    for (std::size_t i = 1; i < endpoints.size(); ++i) {
        const double lo = endpoints[i - 1], hi = endpoints[i];
        const auto p = center + axis * ((lo + hi) * 0.5);
        const double phi = normalize(std::atan2(p.y, p.x) - sector.phiStart);
        if (phi >= sector.phiDelta) {
            if (!kept.empty() && kept.back().hi == lo)
                kept.back().hi = hi;
            else
                kept.push_back({lo, hi});
        }
    }
    return kept;
}
} // namespace nodehammer::tessellation
