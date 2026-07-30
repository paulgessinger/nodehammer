#include <ir/dd4hep/semantic/importer.hpp>
#include <diagnostic_codes.hpp>
#include <ir/provenance.hpp>
#include <ir/tgeo/semantic/importer.hpp>

#include <DD4hep/DetElement.h>
#include <DD4hep/Detector.h>
#include <DD4hep/Printout.h>
#include <DD4hep/Volumes.h>

#include <TGeoManager.h>
#include <TGeoNode.h>
#include <TGeoVolume.h>

#include <RtypesCore.h> // gErrorIgnoreLevel

#include <format>
#include <print>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer::ir {

namespace {

// Walk the DD4hep DetElement tree and annotate the SemanticNodes
// that were already created by the TGeo pass.
void annotateDetElement(const dd4hep::DetElement &elem, semantic::Scene &scene,
                        DiagnosticList &diags,
                        const std::unordered_map<const TGeoNode *, semantic::NodeId> &nodeMap) {
    const TGeoNode *geoNode = elem.placement().ptr();
    auto it = nodeMap.find(geoNode);
    if (it == nodeMap.end()) {
        diags.warn(codes::kWarnImportNoMaterial,
                   std::format("DetElement '{}' placement not found in TGeo tree", elem.name()));
        return;
    }

    semantic::Node &sn = scene.nodes[it->second];

    // Override name and sourceSystem with the richer DD4hep information.
    sn.name = elem.name();
    sn.sourceSystem = "dd4hep";

    if (!std::string{elem.type()}.empty()) {
        sn.tags["subdetector"] = elem.type();
    }

    // Sensitivity is primarily tagged during the post-pass below via
    // ddVol.isSensitive(), but a DetElement may also carry it on its own volume.
    if (elem.volume().isSensitive()) {
        sn.tags["sensitive"] = "true";
    }

    for (const auto &[childName, childElem] : elem.children()) {
        annotateDetElement(childElem, scene, diags, nodeMap);
    }
}

// Tag sensitivity on all nodes by checking DD4hep volume metadata.
// This catches sensitive volumes that have no corresponding DetElement.
//
// We work via lvMap (TGeoVolume* → semantic::LogVolId) rather than nodeMap
// (TGeoNode* → semantic::NodeId) because multiple SemanticNodes can originate
// from the same TGeoNode when a parent volume is placed more than once.
// The nodeMap only stores the last such mapping, so iterating it would miss
// most placements.  The lvMap is 1:1 and lets us tag every node whose
// logical volume is sensitive.
void tagSensitiveVolumes(semantic::Scene &scene,
                         const std::unordered_map<const TGeoVolume *, semantic::LogVolId> &lvMap) {
    // Step 1: collect the set of sensitive logVolIds.
    std::unordered_set<semantic::LogVolId> sensitiveLogVols;
    for (const auto &[vol, logVolId] : lvMap) {
        if (vol == nullptr) {
            continue;
        }
        dd4hep::Volume ddVol{const_cast<TGeoVolume *>(vol)};
        if (ddVol.isSensitive()) {
            sensitiveLogVols.insert(logVolId);
        }
    }

    // Step 2: tag every node that references a sensitive logVol.
    for (auto &[_, node] : scene.nodes) {
        if (sensitiveLogVols.contains(node.logVolId)) {
            node.tags["sensitive"] = "true";
        }
    }
}

} // namespace

std::string_view DD4hepImporter::formatName() const noexcept { return "dd4hep"; }

std::vector<std::string> DD4hepImporter::supportedExtensions() const { return {}; }

ImportResult DD4hepImporter::import(const std::filesystem::path &path) const {
    // Suppress ROOT and DD4hep noise so stdout stays clean for JSON piping.
    const int savedRootLevel = gErrorIgnoreLevel;
    const dd4hep::PrintLevel savedDd4hepLevel = dd4hep::printLevel();
    gErrorIgnoreLevel = kError;
    dd4hep::setPrintLevel(dd4hep::ERROR);

    std::unique_ptr<dd4hep::Detector> detOwner;
    try {
        detOwner = dd4hep::Detector::make_unique("");
        detOwner->fromCompact(path.string());
    } catch (const std::exception &ex) {
        ImportResult result;
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("DD4hep failed to load '{}': {}", path.string(), ex.what()));
        gErrorIgnoreLevel = savedRootLevel;
        dd4hep::setPrintLevel(savedDd4hepLevel);
        return result;
    }

    // Pass 1: full TGeo tree traversal — every node gets a semantic::Node.
    auto tr = traverseTGeoManager(&detOwner->manager(), path.string());

    // Pass 2: tag sensitivity on all volumes using DD4hep metadata.
    tagSensitiveVolumes(tr.result.scene, tr.lvMap);

    // Pass 3: walk the DD4hep DetElement tree and annotate nodes with richer
    // names, provenance, and metadata tags (subdetector type, etc.).
    const dd4hep::DetElement world = detOwner->world();
    annotateDetElement(world, tr.result.scene, tr.result.diags, tr.nodeMap);

    // Update sourceSystem for nodes not touched by annotateDetElement.
    for (auto &[_, node] : tr.result.scene.nodes) {
        if (node.sourceSystem == "tgeo") {
            node.sourceSystem = "dd4hep/tgeo";
        }
    }

    std::println(stderr, "DD4hep import stats:");
    std::println(stderr, "  TGeo nodes (semantic nodes): {}", tr.result.scene.nodes.size());
    std::println(stderr, "  Semantic logvols:             {}", tr.result.scene.logVols.size());
    std::println(stderr, "  Semantic shapes:              {}", tr.result.scene.shapes.size());
    std::println(stderr, "  Semantic materials:           {}", tr.result.scene.materials.size());

    gErrorIgnoreLevel = savedRootLevel;
    dd4hep::setPrintLevel(savedDd4hepLevel);
    return std::move(tr.result);
}

} // namespace nodehammer::ir
