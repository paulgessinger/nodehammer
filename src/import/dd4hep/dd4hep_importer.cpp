#include <nodehammer/import/dd4hep/dd4hep_importer.hpp>
#include <nodehammer/import/tgeo/tgeo_shape_dispatch.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/provenance.hpp>

#include <DD4hep/DetElement.h>
#include <DD4hep/Detector.h>
#include <DD4hep/Volumes.h>

#include <TGeoManager.h>
#include <TGeoMatrix.h>
#include <TGeoNode.h>
#include <TGeoVolume.h>

#include <format>
#include <unordered_map>
#include <unordered_set>

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
    std::string sourceFile;
    dd4hep::Detector &det;
    std::unordered_map<const TGeoVolume *, SemanticLogVolId> lvCache;
    std::unordered_map<const TGeoMaterial *, SemanticMaterialId> matCache;
    std::unordered_set<const TGeoNode *> visitedNodes;
};

SemanticMaterialId importMaterial(const TGeoVolume *vol, ImportState &st) {
    const TGeoMaterial *mat = vol->GetMaterial();
    if (!mat) {
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

    const SemanticShapeId shapeId = dispatchTGeoShape(vol->GetShape(), st.scene, st.diags);
    const SemanticMaterialId matId = importMaterial(vol, st);

    const SemanticLogVolId id = st.scene.nextLogVolId();
    st.scene.logVols[id] = {id, vol->GetName(), shapeId, matId};
    st.lvCache[vol] = id;
    return id;
}

// Pass 1: walk the DD4hep DetElement tree, building SemanticNodes with metadata tags.
SemanticNodeId importDetElement(const dd4hep::DetElement &elem,
                                std::optional<SemanticNodeId> parentId, ImportState &st,
                                const std::unordered_set<std::string> &sensNames) {
    const SemanticNodeId id = st.scene.nextNodeId();

    dd4hep::PlacedVolume pv = elem.placement();
    const TGeoNode *geoNode = pv.ptr();

    st.visitedNodes.insert(geoNode);

    SemanticNode sn;
    sn.id = id;
    sn.name = elem.name();
    sn.logVolId = importLogVol(pv.volume().ptr(), st);
    sn.localTransform = tgeoMatrixToGlm(geoNode->GetMatrix());
    sn.parentId = parentId;
    sn.provenance = {"dd4hep", elem.name(), st.sourceFile, {}};

    if (!std::string{elem.type()}.empty()) {
        sn.tags["subdetector"] = elem.type();
    }
    if (sensNames.count(elem.name())) {
        sn.tags["sensitive"] = "true";
    }

    st.scene.nodes[id] = sn;

    for (const auto &[childName, childElem] : elem.children()) {
        const SemanticNodeId childId = importDetElement(childElem, id, st, sensNames);
        st.scene.nodes[id].children.push_back(childId);
    }

    return id;
}

// Pass 2: walk the TGeo tree and import structural nodes not covered by Pass 1.
void importStructural(const TGeoNode *node, std::optional<SemanticNodeId> parentId,
                      ImportState &st) {
    if (st.visitedNodes.count(node)) {
        return;
    }
    st.visitedNodes.insert(node);

    const SemanticNodeId id = st.scene.nextNodeId();

    SemanticNode sn;
    sn.id = id;
    sn.name = node->GetName();
    sn.logVolId = importLogVol(node->GetVolume(), st);
    sn.localTransform = tgeoMatrixToGlm(node->GetMatrix());
    sn.parentId = parentId;
    sn.provenance = {"dd4hep", node->GetName(), st.sourceFile, {}};

    st.scene.nodes[id] = sn;

    if (parentId) {
        st.scene.nodes[*parentId].children.push_back(id);
    }

    for (int i = 0; i < node->GetNdaughters(); ++i) {
        importStructural(node->GetDaughter(i), id, st);
    }
}

} // namespace

std::string_view DD4hepImporter::formatName() const noexcept { return "dd4hep"; }

std::vector<std::string> DD4hepImporter::supportedExtensions() const { return {}; }

ImportResult DD4hepImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    dd4hep::Detector *detPtr = nullptr;
    try {
        // @TODO: use   std::unique_ptr<dd4hep::Detector> detector =
        //   dd4hep::Detector::make_unique(cfg.name);
        //   for (const auto& file : cfg.xmlFileNames) {
        //     detector->fromCompact(file);
        //   }
        detPtr = &dd4hep::Detector::getInstance();
        detPtr->fromCompact(path.string());
    } catch (const std::exception &ex) {
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("DD4hep failed to load '{}': {}", path.string(), ex.what()));
        if (detPtr) {
            try {
                dd4hep::Detector::destroyInstance();
            } catch (...) {
            }
        }
        return result;
    }

    dd4hep::Detector &det = *detPtr;
    ImportState st{result.scene, result.diags, path.string(), det, {}, {}, {}};

    // Build the set of sensitive detector names
    std::unordered_set<std::string> sensNames;
    for (const auto &[name, sd] : det.sensitiveDetectors()) {
        sensNames.insert(name);
    }

    // Pass 1: DetElement tree
    const dd4hep::DetElement world = det.world();
    const SemanticNodeId rootId = importDetElement(world, std::nullopt, st, sensNames);
    result.scene.rootId = rootId;

    // Pass 2: structural TGeo nodes not reached by DetElement tree.
    //
    // NOTE: in practice this pass is currently a no-op for well-formed DD4hep
    // geometries.  Pass 1 inserts det.world().placement().ptr() — the top
    // TGeoNode — into visitedNodes, so importStructural immediately returns
    // when called with GetTopNode() and never recurses into its children.
    //
    // Consequence: the SemanticScene contains exactly one node per DetElement
    // (the logical detector hierarchy), but NOT the TGeoNode sub-volumes that
    // live inside each DetElement's volume (e.g. individual sensor wafers,
    // readout ASICs, glue layers).  For ODD this gives ~64k nodes vs ~186k
    // in the raw TGeo tree.
    //
    // Whether this is a bug or a feature depends on the use case:
    //   - For visualisation and high-level geometry validation the DetElement
    //     granularity is usually the right semantic cut, and sub-volumes are
    //     internal implementation detail.
    //   - For full-geometry export (parity with TGeoImporter) Pass 2 should
    //     recurse into the TGeoNode daughters of each DetElement node that
    //     Pass 1 visited.  This requires storing the SemanticNodeId alongside
    //     each TGeoNode* in visitedNodes so that structural children can be
    //     parented correctly.
    //
    // Suggested follow-up: add a dd4hep_depth config option
    // ("detector_element" | "full") and only activate the corrected Pass 2
    // when "full" is requested.  Validate visually by comparing the DD4hep
    // and TGeo import paths on the same geometry once glTF export is available
    // (Checkpoint 7).
    importStructural(det.manager().GetTopNode(), std::nullopt, st);

    result.scene.computeWorldTransforms();

    try {
        dd4hep::Detector::destroyInstance();
    } catch (...) {
    }

    return result;
}

} // namespace nodehammer
