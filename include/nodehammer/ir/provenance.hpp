#pragma once

#include <bitset>
#include <cstddef>
#include <string>

namespace nodehammer {

/// Bit positions for degradation flags. Pass to DegradationFlags::set()/has().
/// @TODO: Revisit if we even need this
enum class DegradationBit : std::size_t {
    UnknownShape = 0,    ///< Shape type not recognized; placeholder emitted
    MaterialMissing = 1, ///< No material info available
    TransformApprox = 2, ///< Transform decomposed with approximation
    TruncatedName = 3,   ///< Node name was truncated
    Count_               ///< Sentinel — not a real flag
};

/// Bitmask recording what information was lost or degraded during import.
struct DegradationFlags {
    std::bitset<static_cast<std::size_t>(DegradationBit::Count_)> bits;

    void set(DegradationBit bit) noexcept { bits.set(static_cast<std::size_t>(bit)); }

    [[nodiscard]] bool has(DegradationBit bit) const noexcept {
        return bits.test(static_cast<std::size_t>(bit));
    }

    [[nodiscard]] bool any() const noexcept { return bits.any(); }
    [[nodiscard]] bool none() const noexcept { return bits.none(); }
};

/// Records the origin of a node in the source geometry system.
struct Provenance {
    std::string sourceSystem; ///< e.g. "tgeo", "dd4hep", "gdml", "synthetic"
    std::string sourceName;   ///< Original name in the source system (volume, DetElement, etc.)
    std::string sourceFile;   ///< Source file path (empty if unavailable)
    DegradationFlags degradation;
};

} // namespace nodehammer
