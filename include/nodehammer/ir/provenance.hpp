#pragma once

#include <cstdint>
#include <string>

namespace nodehammer {

/// Bitmask flags recording what information was lost or degraded during import.
struct DegradationFlags {
    static constexpr uint32_t None = 0;
    static constexpr uint32_t UnknownShape =
        1u << 0; ///< Shape type not recognized; placeholder emitted
    static constexpr uint32_t MaterialMissing = 1u << 1; ///< No material info available
    static constexpr uint32_t TransformApprox = 1u
                                                << 2;  ///< Transform decomposed with approximation
    static constexpr uint32_t TruncatedName = 1u << 3; ///< Node name was truncated

    uint32_t value{None};

    void set(uint32_t flag) noexcept { value |= flag; }
    [[nodiscard]] bool has(uint32_t flag) const noexcept { return (value & flag) != 0; }
};

/// Records the origin of a node in the source geometry system.
struct Provenance {
    std::string sourceSystem; ///< e.g. "tgeo", "dd4hep", "gdml", "synthetic"
    std::string sourceName;   ///< Original name in the source system (volume, DetElement, etc.)
    std::string sourceFile;   ///< Source file path (empty if unavailable)
    DegradationFlags degradation;
};

} // namespace nodehammer
