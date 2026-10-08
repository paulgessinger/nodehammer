#include <diagnostic_codes.hpp>
#include <ir/expanded/conversion.hpp>
#include <ir/provenance.hpp>
#include <ir/tgeo/semantic/importer.hpp>
#include <ir/tgeo/semantic/shape_dispatch.hpp>
#include <ir/tgeo/semantic/shared_importer.hpp>

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

namespace nodehammer::ir {

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
    semantic::GeometryCatalogs &scene;
    DiagnosticList &diags;
    std::unordered_map<const TGeoVolume *, semantic::LogVolId> lvCache;
    std::unordered_map<const TGeoShape *, semantic::ShapeId> shapeCache;
    std::unordered_map<const TGeoMaterial *, semantic::MaterialId> matCache;
    std::unordered_map<const TGeoNode *, expanded::NodeId> nodeMap;
};

semantic::MaterialId importMaterial(const TGeoVolume *vol, ImportState &st) {
    const TGeoMaterial *mat = vol->GetMaterial();
    if (mat == nullptr) {
        st.diags.warn(codes::kWarnImportNoMaterial,
                      std::format("volume '{}' has no material", vol->GetName()));
        const semantic::MaterialId id = st.scene.nextMaterialId();
        st.scene.materials[id] = {id, "<none>", std::nullopt, 0.0};
        return id;
    }

    auto it = st.matCache.find(mat);
    if (it != st.matCache.end()) {
        return it->second;
    }

    const semantic::MaterialId id = st.scene.nextMaterialId();
    semantic::SourceMaterial sm;
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

semantic::LogVolId importLogVol(const TGeoVolume *vol, ImportState &st) {
    auto it = st.lvCache.find(vol);
    if (it != st.lvCache.end()) {
        return it->second;
    }

    const TGeoShape *geoShape = vol->GetShape();
    auto sit = st.shapeCache.find(geoShape);
    semantic::ShapeId shapeId;
    if (sit != st.shapeCache.end()) {
        shapeId = sit->second;
    } else {
        shapeId = dispatchTGeoShape(geoShape, st.scene, st.diags);
        st.shapeCache[geoShape] = shapeId;
    }
    const semantic::MaterialId matId = importMaterial(vol, st);

    const semantic::LogVolId id = st.scene.nextLogVolId();
    st.scene.logVols[id] = {id, vol->GetName(), shapeId, matId};
    st.lvCache[vol] = id;

    std::vector<semantic::DaughterPlacement> daughters;
    for (int i = 0; i < vol->GetNdaughters(); ++i) {
        const TGeoNode *daughter = vol->GetNode(i);
        if (daughter == nullptr || daughter->GetVolume() == nullptr) {
            continue;
        }
        daughters.push_back(semantic::DaughterPlacement{daughter->GetName(),
                                                        importLogVol(daughter->GetVolume(), st),
                                                        tgeoMatrixToGlm(daughter->GetMatrix())});
    }
    st.scene.logVols.at(id).daughters = std::move(daughters);
    return id;
}

expanded::NodeId importNode(const TGeoNode *node, std::optional<expanded::NodeId> parentId,
                            ImportState &st, expanded::Scene &scene) {
    const expanded::NodeId id = scene.nextNodeId();
    st.nodeMap[node] = id;

    expanded::Node sn;
    sn.id = id;
    sn.name = node->GetName();
    sn.logVolId = importLogVol(node->GetVolume(), st);
    sn.localTransform = tgeoMatrixToGlm(node->GetMatrix());
    sn.parentId = parentId;
    sn.sourceSystem = "tgeo";

    scene.nodes[id] = sn;

    for (int i = 0; i < node->GetNdaughters(); ++i) {
        const TGeoNode *child = node->GetDaughter(i);
        const expanded::NodeId childId = importNode(child, id, st, scene);
        scene.nodes[id].children.push_back(childId);
    }

    return id;
}

} // namespace

semantic::detail::TGeoSourceIndex semantic::detail::extractTGeoSource(TGeoManager *mgr,
                                                                      std::string sourceFile) {
    if (mgr == nullptr || mgr->GetTopNode() == nullptr || mgr->GetTopNode()->GetVolume() == nullptr)
        throw Error{codes::kFatalTgeoOpenFailed, "missing TGeo manager or root placement",
                    sourceFile};
    if (sourceFile.empty())
        sourceFile = mgr->GetName();
    TGeoSourceIndex result;
    result.result.scene.sourceFile = std::move(sourceFile);
    ImportState state{result.result.scene, result.result.diags, {}, {}, {}, {}};
    result.rootNode = mgr->GetTopNode();
    result.result.scene.root = {result.rootNode->GetName(),
                                importLogVol(result.rootNode->GetVolume(), state), glm::dmat4{1.0}};
    result.result.scene.metadata.defaultSourceSystem = "tgeo";
    result.volumes = std::move(state.lvCache);
    for (const auto &[volume, id] : result.volumes) {
        std::size_t emitted = 0;
        for (int i = 0; i < volume->GetNdaughters(); ++i) {
            const auto *daughter = volume->GetNode(i);
            if (daughter != nullptr && daughter->GetVolume() != nullptr)
                result.placements[daughter].emplace_back(id, emitted++);
        }
    }
    return result;
}

TGeoTraversalResult traverseTGeoManager(TGeoManager *mgr, std::string sourceFile) {
    TGeoTraversalResult tr;
    tr.result.scene.sourceFile = std::move(sourceFile);
    ImportState st{tr.result.scene, tr.result.diags, {}, {}, {}, {}};

    TGeoNode *topNode = mgr->GetTopNode();
    const expanded::NodeId rootId = importNode(topNode, std::nullopt, st, tr.result.scene);
    tr.result.scene.rootId = rootId;
    tr.result.scene.nodes[rootId].localTransform = glm::dmat4{1.0}; // top node is at origin

    tr.result.scene.computeWorldTransforms();
    tr.result.scene.computeOriginalPaths();
    tr.nodeMap = std::move(st.nodeMap);
    tr.lvMap = std::move(st.lvCache);
    return tr;
}

std::string_view TGeoImporter::formatName() const noexcept { return "tgeo"; }

std::vector<std::string> TGeoImporter::supportedExtensions() const { return {".root"}; }

expanded::ImportResult TGeoImporter::importExpanded(TGeoManager *mgr) const {
    if (mgr == nullptr) {
        throw Error{codes::kFatalTgeoOpenFailed, "null TGeoManager pointer", "TGeoImporter"};
    }
    return traverseTGeoManager(mgr, mgr->GetName()).result;
}

ImportResult TGeoImporter::import(const std::filesystem::path &path) const {
    try {
        const int savedLevel = gErrorIgnoreLevel;
        struct RestoreLevel {
            int value;
            ~RestoreLevel() { gErrorIgnoreLevel = value; }
        } restore{savedLevel};
        gErrorIgnoreLevel = kError;
        auto *manager = TGeoManager::Import(path.c_str());
        if (!manager)
            throw Error{codes::kFatalTgeoOpenFailed, "failed to open ROOT geometry", path.string()};
        auto result = semantic::importTGeo(manager, path.string());
        return result;
    } catch (const Error &) {
        throw;
    } catch (const std::exception &ex) {
        throw Error{codes::kFatalTgeoOpenFailed, ex.what(), path.string()};
    }
}

ImportResult TGeoImporter::import(TGeoManager *manager) const {
    return semantic::importTGeo(manager);
}
} // namespace nodehammer::ir
