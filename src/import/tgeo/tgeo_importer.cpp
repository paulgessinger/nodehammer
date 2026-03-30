#include <nodehammer/import/tgeo/tgeo_importer.hpp>
#include <nodehammer/import/tgeo/tgeo_shape_dispatch.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/provenance.hpp>

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
    std::string sourceFile;
    std::unordered_map<const TGeoVolume *, SemanticLogVolId> lvCache;
    std::unordered_map<const TGeoMaterial *, SemanticMaterialId> matCache;
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

    const SemanticShapeId shapeId = dispatchTGeoShape(vol->GetShape(), st.scene, st.diags);
    const SemanticMaterialId matId = importMaterial(vol, st);

    const SemanticLogVolId id = st.scene.nextLogVolId();
    st.scene.logVols[id] = {id, vol->GetName(), shapeId, matId};
    st.lvCache[vol] = id;
    return id;
}

// Forward declaration for recursion
SemanticNodeId importNode(const TGeoNode *node, std::optional<SemanticNodeId> parentId,
                          ImportState &st);

SemanticNodeId importNode(const TGeoNode *node, std::optional<SemanticNodeId> parentId,
                          ImportState &st) {
    const SemanticNodeId id = st.scene.nextNodeId();

    SemanticNode sn;
    sn.id = id;
    sn.name = node->GetName();
    sn.logVolId = importLogVol(node->GetVolume(), st);
    sn.localTransform = tgeoMatrixToGlm(node->GetMatrix());
    sn.parentId = parentId;
    sn.provenance = {"tgeo", node->GetName(), st.sourceFile, {}};

    st.scene.nodes[id] = sn;

    for (int i = 0; i < node->GetNdaughters(); ++i) {
        const TGeoNode *child = node->GetDaughter(i);
        const SemanticNodeId childId = importNode(child, id, st);
        st.scene.nodes[id].children.push_back(childId);
    }

    return id;
}

ImportResult traverseManager(TGeoManager *mgr, std::string sourceFile) {
    ImportResult result;
    ImportState st{result.scene, result.diags, std::move(sourceFile), {}, {}};

    TGeoNode *topNode = mgr->GetTopNode();
    const SemanticNodeId rootId = importNode(topNode, std::nullopt, st);
    result.scene.rootId = rootId;
    result.scene.nodes[rootId].localTransform = glm::dmat4{1.0}; // top node is at origin

    result.scene.computeWorldTransforms();
    return result;
}

} // namespace

std::string_view TGeoImporter::formatName() const noexcept { return "tgeo"; }

std::vector<std::string> TGeoImporter::supportedExtensions() const { return {".root"}; }

ImportResult TGeoImporter::import(const std::filesystem::path &path) const {
    const int savedLevel = gErrorIgnoreLevel;
    gErrorIgnoreLevel = kError;
    TGeoManager *mgr = TGeoManager::Import(path.c_str());
    gErrorIgnoreLevel = savedLevel;

    if (!mgr) {
        ImportResult result;
        result.diags.error(codes::kErrTgeoOpenFailed,
                           std::format("failed to open ROOT file '{}'", path.string()));
        return result;
    }

    return traverseManager(mgr, path.string());
}

ImportResult TGeoImporter::import(TGeoManager *mgr) const {
    if (!mgr) {
        ImportResult result;
        result.diags.error(codes::kErrTgeoOpenFailed, "null TGeoManager pointer");
        return result;
    }
    return traverseManager(mgr, mgr->GetName());
}

} // namespace nodehammer
