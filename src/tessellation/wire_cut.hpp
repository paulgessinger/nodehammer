#pragma once

#include <ir/semantic.hpp>
#include <optional>
#include <vector>

namespace nodehammer::tessellation {
struct WireInterval {
    double lo;
    double hi;
};

// Approximate a slender solid cylinder minus a surrounding azimuthal sector by
// capped axial spans. nullopt requires the exact Boolean path (unsupported or
// grazing geometry). Empty vector means fully removed. Returned endpoints are
// in the original cylinder's local z coordinate.
std::optional<std::vector<WireInterval>> clipWireCenterline(const ir::semantic::TubeShape &wire,
                                                            const ir::semantic::TubeShape &sector,
                                                            const glm::dmat4 &wireToSector);

bool isStraightSlenderWire(const ir::semantic::ShapeVariant &shape);
} // namespace nodehammer::tessellation
