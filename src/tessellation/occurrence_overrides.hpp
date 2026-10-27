#pragma once

#include <ir/expanded/scene.hpp>

namespace nodehammer::tessellation {

inline constexpr const char *kWedgeCutTag = "nodehammer_wedge_cut";

// Transient edits keyed by preorder occurrence IDs in one unchanged prototype
// graph. Added cut volumes are geometry-only: they never replace source topology.
// The source scene and this overlay must outlive the tessellation job. Do not
// reuse an overlay after selecting, deduplicating or otherwise editing topology.
struct OccurrenceOverrides {
    ankerl::unordered_dense::map<ir::expanded::NodeId, ir::semantic::LogVolId> volumes;
    std::vector<bool> pruned;
    std::optional<ir::semantic::ShapeId> wedgeShapeId;

    [[nodiscard]] bool excludes(ir::expanded::NodeId id) const {
        return id.value < pruned.size() && pruned[static_cast<std::size_t>(id.value)];
    }
};

} // namespace nodehammer::tessellation
