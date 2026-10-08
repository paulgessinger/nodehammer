#pragma once

#include <ir/semantic/importer.hpp>

#include <string>
#include <unordered_map>

class TGeoManager;
class TGeoNode;
class TGeoVolume;

namespace nodehammer::ir::semantic {

/// Borrow a live manager and retain its unique source definitions, without
/// expanding physical occurrences or changing the manager's ownership.
[[nodiscard]] ir::ImportResult importTGeo(TGeoManager *manager, std::string sourceFile = {});

namespace detail {
// Transient exact source identities used by DD4hep annotation. Pointers never
// enter Scene and are only valid while the caller's manager is alive.
struct TGeoSourceIndex {
    ir::ImportResult result;
    const TGeoNode *rootNode{};
    std::unordered_map<const TGeoVolume *, semantic::LogVolId> volumes;
    std::unordered_map<const TGeoNode *, std::vector<PlacementKey>> placements;
};
[[nodiscard]] TGeoSourceIndex extractTGeoSource(TGeoManager *manager, std::string sourceFile);
} // namespace detail

} // namespace nodehammer::ir::semantic
