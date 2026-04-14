#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/provenance.hpp>
#include <nodehammer/ir/tgeo/semantic/importer.hpp>
#include <nodehammer/ir/tgeo/semantic/shape_dispatch.hpp>

#include <TColor.h>
#include <TError.h>
#include <TGeoManager.h>
#include <TGeoMaterial.h>
#include <TGeoMatrix.h>
#include <TGeoNode.h>
#include <TGeoVolume.h>
#include <TROOT.h>

#include <format>
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
    std::unordered_map<const TGeoVolume *, SemanticLogVolId> lvCache;
    std::unordered_map<const TGeoShape *, SemanticShapeId> shapeCache;
    std::unordered_map<const TGeoMaterial *, SemanticMaterialId> matCache;
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
    SourceMaterial sm;
    sm.id = id;
    sm.name = mat->GetName();
    sm.density = mat->GetDensity();

    // Color from the volume's line color (ROOT color index)
    const int colorIdx = vol->GetLineColor();
    if (const TColor *col = gROOT->GetColor(colorIdx)) {
        sm.color = glm::vec3{static_cast<float>(col->GetRed()), static_cast<float>(col->GetGreen()),
                             static_cast<float>(col->GetBlue())};
    }

    st.scene.materials[id] = sm;
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

SemanticNodeId importNode(const TGeoNode *node, std::optional<SemanticNodeId> parentId,
                          ImportState &st) {
    const SemanticNodeId id = st.scene.nextNodeId();
    st.nodeMap[node] = id;

    SemanticNode sn;
    sn.id = id;
    sn.name = node->GetName();
    sn.logVolId = importLogVol(node->GetVolume(), st);
    sn.localTransform = tgeoMatrixToGlm(node->GetMatrix());
    sn.parentId = parentId;
    sn.sourceSystem = "tgeo";

    st.scene.nodes[id] = sn;

    for (int i = 0; i < node->GetNdaughters(); ++i) {
        const TGeoNode *child = node->GetDaughter(i);
        const SemanticNodeId childId = importNode(child, id, st);
        st.scene.nodes[id].children.push_back(childId);
    }

    return id;
}

} // namespace

TGeoTraversalResult traverseTGeoManager(TGeoManager *mgr, std::string sourceFile) {
    TGeoTraversalResult tr;
    tr.result.scene.sourceFile = std::move(sourceFile);
    ImportState st{tr.result.scene, tr.result.diags, {}, {}, {}, {}};

    TGeoNode *topNode = mgr->GetTopNode();
    const SemanticNodeId rootId = importNode(topNode, std::nullopt, st);
    tr.result.scene.rootId = rootId;
    tr.result.scene.nodes[rootId].localTransform = glm::dmat4{1.0}; // top node is at origin

    tr.result.scene.computeWorldTransforms();
    tr.result.scene.computeOriginalPaths();
    tr.nodeMap = std::move(st.nodeMap);
    return tr;
}

std::string_view TGeoImporter::formatName() const noexcept { return "tgeo"; }

std::vector<std::string> TGeoImporter::supportedExtensions() const { return {".root"}; }

ImportResult TGeoImporter::import(const std::filesystem::path &path) const {
    const int savedLevel = gErrorIgnoreLevel;
    gErrorIgnoreLevel = kError;
    TGeoManager *mgr = TGeoManager::Import(path.c_str());
    gErrorIgnoreLevel = savedLevel;

    if (mgr == nullptr) {
        ImportResult result;
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("failed to open ROOT file '{}'", path.string()));
        return result;
    }

    return traverseTGeoManager(mgr, path.string()).result;
}

ImportResult TGeoImporter::import(TGeoManager *mgr) const {
    if (mgr == nullptr) {
        ImportResult result;
        result.diags.error(codes::kErrTgeoOpenFailed, "null TGeoManager pointer");
        return result;
    }
    return traverseTGeoManager(mgr, mgr->GetName()).result;
}

} // namespace nodehammer
