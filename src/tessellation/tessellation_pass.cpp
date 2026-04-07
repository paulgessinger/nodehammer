#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <format>
#include <queue>
#include <unordered_map>

namespace nodehammer {

namespace {

// Build BFS paths (same as SelectionEngine::buildPaths — duplicated here to
// avoid a cross-module dependency; both are internal implementation details).
std::unordered_map<SemanticNodeId, std::string> buildPaths(const SemanticScene &scene) {
    std::unordered_map<SemanticNodeId, std::string> paths;
    if (scene.nodes.empty() || !scene.nodes.contains(scene.rootId)) {
        return paths;
    }
    paths.reserve(scene.nodes.size());
    paths[scene.rootId] = "/" + scene.nodes.at(scene.rootId).name;
    std::queue<SemanticNodeId> q;
    q.push(scene.rootId);
    while (!q.empty()) {
        const auto &node = scene.nodes.at(q.front());
        q.pop();
        for (const auto childId : node.children) {
            if (!scene.nodes.contains(childId))
                continue;
            paths[childId] = paths.at(node.id) + "/" + scene.nodes.at(childId).name;
            q.push(childId);
        }
    }
    return paths;
}

// Return the TessellationRule that applies to nodePath (first scope match, or
// last rule if none match, or a default if the vector is empty).
const TessellationRule &resolveRule(const std::vector<TessellationRule> &rules,
                                    const std::string &nodePath) {
    static const TessellationRule kDefault;
    if (rules.empty())
        return kDefault;
    for (const auto &r : rules) {
        if (!r.scope.has_value() || matchGlob(*r.scope, nodePath)) {
            return r;
        }
    }
    return rules.back();
}

// Return the first MaterialRule that matches, or nullptr.
const MaterialRule *resolveMaterial(const std::vector<MaterialRule> &matRules,
                                    const std::string &nodePath, const NodeView &view) {
    for (const auto &mr : matRules) {
        if (mr.scope.has_value() && !matchGlob(*mr.scope, nodePath))
            continue;
        if (mr.match.has_value()) {
            auto pred = compilePredicate(*mr.match);
            if (!pred(view))
                continue;
        }
        return &mr;
    }
    return nullptr;
}

// Build a default grey RenderMaterial from a SourceMaterial.
RenderMaterial makeDefaultMaterial(RenderScene &rs, const SourceMaterial &src) {
    RenderMaterial mat;
    mat.id = rs.nextMaterialId();
    mat.name = src.name;
    if (src.color.has_value()) {
        mat.baseColorFactor = glm::vec4{src.color->r, src.color->g, src.color->b, 1.0f};
    }
    return mat;
}

// Axis-aligned bounding box proxy for boolean fallback=BBox.
TessellationOutput makeBBoxProxy() {
    // A unit box placeholder; the actual bbox would require shape-specific
    // computation beyond the scope of a primitive tessellator.
    PrimitiveTessellator pt;
    BoxShape unit{1.0, 1.0, 1.0};
    TessellationParams p;
    return pt.tessellate(unit, p);
}

} // namespace

// ── TessellationPass ──────────────────────────────────────────────────────────

TessellationPass::TessellationPass(const NHConfig &config) : config_(config) {}

TessellationPassResult TessellationPass::lower(const SemanticScene &scene) const {
    TessellationPassResult result;
    if (scene.nodes.empty())
        return result;

    const auto paths = buildPaths(scene);
    PrimitiveTessellator tess;

    // Cache: SemanticMaterialId → RenderMaterialId (avoid creating duplicate RenderMaterials
    // for nodes that share a SourceMaterial and match no named MaterialRule).
    std::unordered_map<SemanticMaterialId, RenderMaterialId> defaultMatCache;
    // Cache: MaterialDef name → RenderMaterialId (avoid duplicating named config materials).
    std::unordered_map<std::string, RenderMaterialId> namedMatCache;
    // Cache: (shapeId, maxSegmentsCircle) → MeshAssetId (LV deduplication).
    std::unordered_map<SemanticShapeId, std::unordered_map<int, MeshAssetId>> meshCache;

    // Map SemanticNodeId → RenderNodeId for parent-linking after creation.
    std::unordered_map<SemanticNodeId, RenderNodeId> nodeMap;

    // BFS order ensures parents are processed before children.
    std::queue<SemanticNodeId> q;
    if (!scene.nodes.contains(scene.rootId))
        return result;
    q.push(scene.rootId);

    while (!q.empty()) {
        const auto semId = q.front();
        q.pop();
        if (!scene.nodes.contains(semId))
            continue;

        const SemanticNode &semNode = scene.nodes.at(semId);
        const std::string &nodePath = paths.contains(semId) ? paths.at(semId) : "";

        // ── Create RenderNode ──────────────────────────────────────────────────
        const RenderNodeId rnId = result.scene.nextNodeId();
        RenderNode rn;
        rn.id = rnId;
        rn.name = semNode.name;
        rn.localTransform = glm::mat4(semNode.localTransform);
        rn.worldTransform = glm::mat4(semNode.worldTransform);
        rn.semanticNodeId = semId;

        if (semNode.parentId.has_value() && nodeMap.contains(*semNode.parentId)) {
            const RenderNodeId parentRnId = nodeMap.at(*semNode.parentId);
            rn.parentId = parentRnId;
            result.scene.nodes.at(parentRnId).children.push_back(rnId);
        }

        nodeMap[semId] = rnId;
        if (!semNode.parentId.has_value()) {
            result.scene.rootId = rnId;
        }

        // ── Resolve tessellation rule ──────────────────────────────────────────
        const TessellationRule &rule = resolveRule(config_.tessellationRules, nodePath);
        TessellationParams params;
        params.maxSegmentsCircle = rule.maxSegmentsCircle;

        // skip_geometry: add the node to the render tree as a structural node but produce no mesh.
        if (rule.skipGeometry) {
            result.scene.nodes[rnId] = rn;
            for (const auto childId : semNode.children)
                q.push(childId);
            continue;
        }

        // ── Resolve shape ──────────────────────────────────────────────────────
        if (!scene.logVols.contains(semNode.logVolId)) {
            result.scene.nodes[rnId] = rn;
            for (const auto childId : semNode.children)
                q.push(childId);
            continue;
        }
        const SemanticLogicalVolume &lv = scene.logVols.at(semNode.logVolId);
        if (!scene.shapes.contains(lv.shapeId)) {
            result.scene.nodes[rnId] = rn;
            for (const auto childId : semNode.children)
                q.push(childId);
            continue;
        }
        const SemanticShape &shape = scene.shapes.at(lv.shapeId);
        const bool isBoolean = std::holds_alternative<BooleanUnion>(shape.data) ||
                               std::holds_alternative<BooleanIntersection>(shape.data) ||
                               std::holds_alternative<BooleanSubtraction>(shape.data);

        // ── Boolean fallback handling ──────────────────────────────────────────
        if (isBoolean) {
            switch (rule.fallback) {
            case BooleanFallback::Fail:
                result.diags.error(
                    codes::kErrTessBooleanFail,
                    std::format("boolean shape on node '{}' cannot be tessellated (fallback=fail)",
                                semNode.name),
                    semNode.name);
                return result; // abort
            case BooleanFallback::BBox: {
                result.diags.warn(
                    codes::kWarnTessBooleanBbox,
                    std::format("boolean shape on node '{}' replaced with bounding-box proxy",
                                semNode.name),
                    semNode.name);
                auto bboxOut = makeBBoxProxy();
                result.diags.append(bboxOut.diags);
                MeshAssetId mid = result.scene.nextMeshId();
                MeshAsset ma;
                ma.id = mid;
                ma.name = semNode.name + "_bbox";
                ma.vertices = std::move(bboxOut.vertices);
                ma.indices = std::move(bboxOut.indices);
                ma.provenance.sourceSystem = "tessellation_pass";
                ma.provenance.sourceName = semNode.name;
                result.scene.meshAssets[mid] = std::move(ma);

                // Resolve material and bind
                const auto &srcMat = scene.materials.at(lv.materialId);
                NodeView view{semNode.name, nodePath, semNode.children.empty(), &semNode.tags};
                const MaterialRule *mr = resolveMaterial(config_.materialRules, nodePath, view);
                RenderMaterialId rmId;
                if (mr) {
                    auto cit = namedMatCache.find(mr->materialName);
                    if (cit != namedMatCache.end()) {
                        rmId = cit->second;
                    } else {
                        for (const auto &md : config_.materials) {
                            if (md.name == mr->materialName) {
                                RenderMaterial rm;
                                rm.id = result.scene.nextMaterialId();
                                rm.name = md.name;
                                rm.baseColorFactor = glm::vec4{md.baseColor.r, md.baseColor.g,
                                                               md.baseColor.b, md.baseColor.a};
                                rm.metallicFactor = md.metallic;
                                rm.roughnessFactor = md.roughness;
                                rm.emissiveFactor =
                                    glm::vec3{md.emissive.r, md.emissive.g, md.emissive.b};
                                rm.doubleSided = md.doubleSided;
                                rmId = rm.id;
                                result.scene.materials[rmId] = std::move(rm);
                                namedMatCache[md.name] = rmId;
                                break;
                            }
                        }
                    }
                }
                if (!result.scene.materials.contains(rmId)) {
                    // Fall back to default
                    if (!defaultMatCache.contains(lv.materialId)) {
                        auto dm = makeDefaultMaterial(result.scene, srcMat);
                        defaultMatCache[lv.materialId] = dm.id;
                        result.scene.materials[dm.id] = std::move(dm);
                    }
                    rmId = defaultMatCache.at(lv.materialId);
                }
                rn.meshBindings.push_back({mid, rmId});
                break;
            }
            case BooleanFallback::Skip:
            default:
                result.diags.warn(
                    codes::kWarnTessBooleanSkipped,
                    std::format("boolean shape on node '{}' skipped (fallback=skip)", semNode.name),
                    semNode.name);
                break;
            }
            result.scene.nodes[rnId] = std::move(rn);
            for (const auto childId : semNode.children)
                q.push(childId);
            continue;
        }

        // ── Tessellate primitive shape ─────────────────────────────────────────
        MeshAssetId mid;
        auto &segMap = meshCache[lv.shapeId];
        if (!segMap.contains(params.maxSegmentsCircle)) {
            auto tessOut = tess.tessellate(shape.data, params);
            result.diags.append(tessOut.diags);
            MeshAsset ma;
            ma.id = result.scene.nextMeshId();
            ma.name = lv.name;
            ma.vertices = std::move(tessOut.vertices);
            ma.indices = std::move(tessOut.indices);
            ma.provenance.sourceSystem = "tessellation_pass";
            ma.provenance.sourceName = lv.name;
            mid = ma.id;
            result.scene.meshAssets[mid] = std::move(ma);
            segMap[params.maxSegmentsCircle] = mid;
        } else {
            mid = segMap.at(params.maxSegmentsCircle);
        }

        // ── Resolve material ───────────────────────────────────────────────────
        const auto &srcMat = scene.materials.at(lv.materialId);
        NodeView view{semNode.name, nodePath, semNode.children.empty(), &semNode.tags};
        const MaterialRule *mr = resolveMaterial(config_.materialRules, nodePath, view);
        RenderMaterialId rmId;
        if (mr) {
            auto cit = namedMatCache.find(mr->materialName);
            if (cit != namedMatCache.end()) {
                rmId = cit->second;
            } else {
                for (const auto &md : config_.materials) {
                    if (md.name == mr->materialName) {
                        RenderMaterial rm;
                        rm.id = result.scene.nextMaterialId();
                        rm.name = md.name;
                        rm.baseColorFactor = glm::vec4{md.baseColor.r, md.baseColor.g,
                                                       md.baseColor.b, md.baseColor.a};
                        rm.metallicFactor = md.metallic;
                        rm.roughnessFactor = md.roughness;
                        rm.emissiveFactor = glm::vec3{md.emissive.r, md.emissive.g, md.emissive.b};
                        rm.doubleSided = md.doubleSided;
                        rmId = rm.id;
                        result.scene.materials[rmId] = std::move(rm);
                        namedMatCache[md.name] = rmId;
                        break;
                    }
                }
            }
        }
        if (!result.scene.materials.contains(rmId)) {
            if (!defaultMatCache.contains(lv.materialId)) {
                auto dm = makeDefaultMaterial(result.scene, srcMat);
                defaultMatCache[lv.materialId] = dm.id;
                result.scene.materials[dm.id] = std::move(dm);
            }
            rmId = defaultMatCache.at(lv.materialId);
        }

        // Only bind mesh if tessellation produced geometry (UnknownShape may be empty)
        if (result.scene.meshAssets.contains(mid) &&
            !result.scene.meshAssets.at(mid).vertices.empty()) {
            rn.meshBindings.push_back({mid, rmId});
        }

        result.scene.nodes[rnId] = std::move(rn);
        for (const auto childId : semNode.children)
            q.push(childId);
    }

    return result;
}

} // namespace nodehammer
