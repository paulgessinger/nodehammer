#pragma once

#include <ir/semantic/importer.hpp>

#include <string_view>
#include <unordered_map>
#include <vector>

class TGeoManager;
class TGeoNode;
class TGeoVolume;

namespace nodehammer {

/// Extended import result that also carries the TGeoNode → SemanticNodeId mapping
/// built during tree traversal. Used by DD4hep to annotate nodes in a second pass.
struct TGeoTraversalResult {
    ImportResult result;
    std::unordered_map<const TGeoNode *, SemanticNodeId> nodeMap;
    std::unordered_map<const TGeoVolume *, SemanticLogVolId> lvMap;
};

/// Walk a TGeoManager and produce a SemanticScene plus the node mapping.
/// Does not modify gGeoManager.
[[nodiscard]] TGeoTraversalResult traverseTGeoManager(TGeoManager *mgr, std::string sourceFile);

/// ISemanticImporter for plain ROOT files containing a TGeoManager.
/// Format name: "tgeo"   Extension: ".root"
class TGeoImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;

    /// Load a .root file. Safe to manipulate gGeoManager; caller accepts that.
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;

    /// Traverse an already-constructed manager. Never touches gGeoManager.
    [[nodiscard]] ImportResult import(TGeoManager *mgr) const;
};

} // namespace nodehammer
