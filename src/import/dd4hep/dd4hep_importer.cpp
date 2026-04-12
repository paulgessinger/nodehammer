#include <nodehammer/import/dd4hep/dd4hep_importer.hpp>
#include <nodehammer/import/tgeo/tgeo_shape_dispatch.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/provenance.hpp>

#include <DD4hep/DetElement.h>
#include <DD4hep/Detector.h>
#include <DD4hep/Printout.h>
#include <DD4hep/Volumes.h>

#include <TGeoManager.h>
#include <TGeoMatrix.h>
#include <TGeoNode.h>
#include <TGeoVolume.h>

#include <RtypesCore.h> // gErrorIgnoreLevel

#include <fcntl.h>
#include <unistd.h>

#include <format>
#include <print>
#include <unordered_map>

namespace nodehammer {

namespace {

glm::dmat4 tgeoMatrixToGlm(const TGeoMatrix *m) {
    const Double_t *r = m->GetRotationMatrix();
    const Double_t *t = m->GetTranslation();
    return glm::dmat4{
        glm::dvec4{r[0], r[3], r[6], 0.0},
        glm::dvec4{r[1], r[4], r[7], 0.0},
        glm::dvec4{r[2], r[5], r[8], 0.0},
        glm::dvec4{t[0], t[1], t[2], 1.0},
    };
}

struct ImportState {
    SemanticScene &scene;
    DiagnosticList &diags;
    dd4hep::Detector &det;
    std::unordered_map<const TGeoVolume *, SemanticLogVolId> lvCache;
    std::unordered_map<const TGeoShape *, SemanticShapeId> shapeCache;
    std::unordered_map<const TGeoMaterial *, SemanticMaterialId> matCache;
    /// TGeoNode* → SemanticNodeId built during the TGeo pass.
    std::unordered_map<const TGeoNode *, SemanticNodeId> nodeMap;
};

SemanticMaterialId importMaterial(const TGeoVolume *vol, ImportState &st) {
    const TGeoMaterial *mat = vol->GetMaterial();
    if (mat == nullptr) {
        st.diags.warn(codes::kWarnImportNoMaterial,
                      std::format("volume '{}' has no material", vol->GetName()));
        const SemanticMaterialId id = st.scene.nextMaterialId();
        st.scene.materials[id] = {id, "<none>", std::nullopt, 0.0};
        return id;
    }

    auto it = st.matCache.find(mat);
    if (it != st.matCache.end()) {
        return it->second;
    }

    const SemanticMaterialId id = st.scene.nextMaterialId();
    st.scene.materials[id] = {id, mat->GetName(), std::nullopt,
                              static_cast<double>(mat->GetDensity())};
    st.matCache[mat] = id;
    return id;
}

SemanticLogVolId importLogVol(const TGeoVolume *vol, ImportState &st) {
    auto it = st.lvCache.find(vol);
    if (it != st.lvCache.end()) {
        return it->second;
    }

    const TGeoShape *geoShape = vol->GetShape();
    auto sit = st.shapeCache.find(geoShape);
    SemanticShapeId shapeId;
    if (sit != st.shapeCache.end()) {
        shapeId = sit->second;
    } else {
        shapeId = dispatchTGeoShape(geoShape, st.scene, st.diags);
        st.shapeCache[geoShape] = shapeId;
    }
    const SemanticMaterialId matId = importMaterial(vol, st);

    const SemanticLogVolId id = st.scene.nextLogVolId();
    st.scene.logVols[id] = {id, vol->GetName(), shapeId, matId};
    st.lvCache[vol] = id;

    std::vector<SemanticDaughterPlacement> daughters;
    for (int i = 0; i < vol->GetNdaughters(); ++i) {
        const TGeoNode *daughter = vol->GetNode(i);
        if (daughter == nullptr || daughter->GetVolume() == nullptr) {
            continue;
        }
        daughters.push_back(SemanticDaughterPlacement{daughter->GetName(),
                                                      importLogVol(daughter->GetVolume(), st),
                                                      tgeoMatrixToGlm(daughter->GetMatrix())});
    }
    st.scene.logVols.at(id).daughters = std::move(daughters);
    return id;
}

// Pass 1: walk the full TGeo tree, creating a SemanticNode for every TGeoNode.
SemanticNodeId importTGeoNode(const TGeoNode *node, std::optional<SemanticNodeId> parentId,
                              ImportState &st) {
    const SemanticNodeId id = st.scene.nextNodeId();
    st.nodeMap[node] = id;

    SemanticNode sn;
    sn.id = id;
    sn.name = node->GetName();
    sn.logVolId = importLogVol(node->GetVolume(), st);
    sn.localTransform = tgeoMatrixToGlm(node->GetMatrix());
    sn.parentId = parentId;
    sn.sourceSystem = "dd4hep/tgeo";

    // Check sensitivity via DD4hep during the TGeo walk — sensitive volumes
    // may not correspond to any DetElement (e.g. composites inside assemblies).
    dd4hep::Volume ddVol{const_cast<TGeoVolume *>(node->GetVolume())};
    if (ddVol.isSensitive()) {
        sn.tags["sensitive"] = "true";
    }

    st.scene.nodes[id] = sn;

    for (int i = 0; i < node->GetNdaughters(); ++i) {
        const SemanticNodeId childId = importTGeoNode(node->GetDaughter(i), id, st);
        st.scene.nodes[id].children.push_back(childId);
    }

    return id;
}

// Pass 2: walk the DD4hep DetElement tree and annotate the SemanticNodes
// that were already created in Pass 1.
void annotateDetElement(const dd4hep::DetElement &elem, ImportState &st) {
    const TGeoNode *geoNode = elem.placement().ptr();
    auto it = st.nodeMap.find(geoNode);
    if (it == st.nodeMap.end()) {
        st.diags.warn(codes::kWarnImportNoMaterial,
                      std::format("DetElement '{}' placement not found in TGeo tree", elem.name()));
        return;
    }

    SemanticNode &sn = st.scene.nodes[it->second];

    // Override name and sourceSystem with the richer DD4hep information.
    sn.name = elem.name();
    sn.sourceSystem = "dd4hep";

    if (!std::string{elem.type()}.empty()) {
        sn.tags["subdetector"] = elem.type();
    }

    // Sensitivity is primarily tagged during the TGeo walk (Pass 1) via
    // ddVol.isSensitive(), but a DetElement may also carry it on its own volume.
    if (elem.volume().isSensitive()) {
        sn.tags["sensitive"] = "true";
    }

    for (const auto &[childName, childElem] : elem.children()) {
        annotateDetElement(childElem, st);
    }
}

} // namespace

std::string_view DD4hepImporter::formatName() const noexcept { return "dd4hep"; }

std::vector<std::string> DD4hepImporter::supportedExtensions() const { return {}; }

ImportResult DD4hepImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

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
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("DD4hep failed to load '{}': {}", path.string(), ex.what()));
        gErrorIgnoreLevel = savedRootLevel;
        dd4hep::setPrintLevel(savedDd4hepLevel);
        return result;
    }

    result.scene.sourceFile = path.string();
    ImportState st{result.scene, result.diags, *detOwner, {}, {}, {}, {}};

    // Pass 1: walk the full TGeo tree — every node gets a SemanticNode.
    // This guarantees complete geometry (passives included) with no duplicates.
    const TGeoNode *topNode = detOwner->manager().GetTopNode();
    const SemanticNodeId rootId = importTGeoNode(topNode, std::nullopt, st);
    result.scene.rootId = rootId;

    // Pass 2: walk the DD4hep DetElement tree and annotate the corresponding
    // SemanticNodes with richer names, provenance, and metadata tags
    // (sensitivity, subdetector type, etc.).
    const dd4hep::DetElement world = detOwner->world();
    annotateDetElement(world, st);

    result.scene.computeWorldTransforms();
    result.scene.computeOriginalPaths();

    std::println(stderr, "DD4hep import stats:");
    std::println(stderr, "  TGeo nodes (semantic nodes): {}", result.scene.nodes.size());
    std::println(stderr, "  Unique TGeoVolume* (logvols): {}", st.lvCache.size());
    std::println(stderr, "  Unique TGeoShape* (shapes):   {}", st.shapeCache.size());
    std::println(stderr, "  Unique TGeoMaterial* (mats):  {}", st.matCache.size());
    std::println(stderr, "  Semantic shapes created:      {}", result.scene.shapes.size());

    gErrorIgnoreLevel = savedRootLevel;
    dd4hep::setPrintLevel(savedDd4hepLevel);
    return result;
}

} // namespace nodehammer
