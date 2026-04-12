#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <nodehammer/tessellation/boolean_tessellator.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <format>
#include <print>
#include <queue>
#include <unordered_map>

namespace nodehammer {

namespace {

// Does a rule's scope/match apply to the given node?
bool ruleMatches(const Rule &r, const NodeView &view) {
    if (r.match.has_value()) {
        auto pred = compilePredicate(*r.match);
        return pred(view);
    }
    return true; // no predicate → matches everything
}

// Return the tessellation settings from the first matching rule that has them,
// or a default if none match.
const Rule::Tessellation &resolveTessellation(const std::vector<Rule> &rules,
                                              const NodeView &view) {
    static const Rule::Tessellation kDefault;
    for (const auto &r : rules) {
        if (!r.tessellation.has_value()) {
            continue;
        }
        if (ruleMatches(r, view)) {
            return *r.tessellation;
        }
    }
    return kDefault;
}

// Return the material name from the first matching rule that has one, or nullptr.
const std::string *resolveMaterial(const std::vector<Rule> &rules, const NodeView &view) {
    for (const auto &r : rules) {
        if (!r.material.has_value()) {
            continue;
        }
        if (ruleMatches(r, view)) {
            return &*r.material;
        }
    }
    return nullptr;
}

// Resolve extras from the first matching rule that has them.
RenderExtrasMap resolveExtras(const std::vector<Rule> &rules, const NodeView &view) {
    for (const auto &r : rules) {
        if (!r.extras.has_value()) {
            continue;
        }
        if (ruleMatches(r, view)) {
            return *r.extras;
        }
    }
    return {};
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
/// Collect all primitive leaf vertices from a (possibly nested) boolean shape,
/// compute their AABB, and return a tessellated box of that size.
TessellationOutput makeBBoxProxy(const SemanticShapeVariant &shapeData, const SemanticScene &scene,
                                 PrimitiveTessellator &tess, const TessellationParams &params) {
    glm::dvec3 bboxMin{std::numeric_limits<double>::max()};
    glm::dvec3 bboxMax{-std::numeric_limits<double>::max()};

    // BFS over the boolean tree, tessellating primitive leaves to find extents.
    std::queue<std::pair<SemanticShapeId, glm::dmat4>> q;

    auto enqueue = [&](const auto &s) {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, BooleanUnion> || std::is_same_v<T, BooleanIntersection> ||
                      std::is_same_v<T, BooleanSubtraction>) {
            q.push({s.left, glm::dmat4{1.0}});
            q.push({s.right, s.rightTransform});
        }
    };
    std::visit(enqueue, shapeData);

    while (!q.empty()) {
        auto [id, xform] = q.front();
        q.pop();
        if (!scene.shapes.contains(id)) {
            continue;
        }
        const auto &child = scene.shapes.at(id).data;
        bool isBool = std::visit(
            [&](const auto &s) {
                using T = std::decay_t<decltype(s)>;
                if constexpr (std::is_same_v<T, BooleanUnion> ||
                              std::is_same_v<T, BooleanIntersection> ||
                              std::is_same_v<T, BooleanSubtraction>) {
                    q.push({s.left, xform});
                    q.push({s.right, xform * s.rightTransform});
                    return true;
                }
                return false;
            },
            child);
        if (!isBool) {
            auto out = tess.tessellate(child, params);
            for (const auto &v : out.vertices) {
                glm::dvec3 p = glm::dvec3(xform * glm::dvec4(v.position, 1.0));
                bboxMin = glm::min(bboxMin, p);
                bboxMax = glm::max(bboxMax, p);
            }
        }
    }

    if (bboxMin.x > bboxMax.x) {
        // No vertices found — unit box fallback.
        return tess.tessellate(BoxShape{1.0, 1.0, 1.0}, params);
    }

    glm::dvec3 half = (bboxMax - bboxMin) * 0.5;
    return tess.tessellate(BoxShape{half.x, half.y, half.z}, params);
}

} // namespace

// ── TessellationPass ──────────────────────────────────────────────────────────

TessellationPass::TessellationPass(const NHConfig &config) : config_(config) {}

TessellationPassResult TessellationPass::lower(const SemanticScene &scene) const {
    TessellationPassResult result;
    if (scene.nodes.empty()) {
        return result;
    }

    PrimitiveTessellator tess;

    // Helper: build a NodeView for a semantic node, resolving its source material name.
    auto makeNodeView = [&](const SemanticNode &node) -> NodeView {
        std::string_view matName;
        if (scene.logVols.contains(node.logVolId)) {
            const auto &lv = scene.logVols.at(node.logVolId);
            if (scene.materials.contains(lv.materialId)) {
                matName = scene.materials.at(lv.materialId).name;
            }
        }
        return {node.name, node.originalPath, matName, node.children.empty(), &node.tags};
    };

    // Lazily-created red material for bbox proxy fallbacks.
    std::optional<RenderMaterialId> bboxProxyMatId;
    auto getBBoxProxyMaterial = [&]() -> RenderMaterialId {
        if (!bboxProxyMatId) {
            RenderMaterial rm;
            rm.id = result.scene.nextMaterialId();
            rm.name = "bbox_proxy";
            rm.baseColorFactor = glm::vec4{0.9f, 0.1f, 0.1f, 1.0f};
            rm.metallicFactor = 0.0f;
            rm.roughnessFactor = 0.8f;
            bboxProxyMatId = rm.id;
            result.scene.materials[rm.id] = std::move(rm);
        }
        return *bboxProxyMatId;
    };

    // Cache: SemanticMaterialId → RenderMaterialId (avoid creating duplicate RenderMaterials
    // for nodes that share a SourceMaterial and match no named material rule).
    std::unordered_map<SemanticMaterialId, RenderMaterialId> defaultMatCache;
    // Cache: MaterialDef name → RenderMaterialId (avoid duplicating named config materials).
    std::unordered_map<std::string, RenderMaterialId> namedMatCache;
    // Cache: (shapeId, maxSegmentsCircle) → MeshAssetId (LV deduplication).
    std::unordered_map<SemanticShapeId, std::unordered_map<int, MeshAssetId>> meshCache;

    // Resolve a SourceMaterial to a RenderMaterialId using the rule system + caches.
    auto resolveRenderMaterial = [&](SemanticMaterialId srcMatId,
                                     const SemanticNode &node) -> RenderMaterialId {
        const auto &srcMat = scene.materials.at(srcMatId);
        NodeView view = makeNodeView(node);
        const std::string *matName = resolveMaterial(config_.rules, view);
        RenderMaterialId rmId;
        if (matName != nullptr) {
            auto cit = namedMatCache.find(*matName);
            if (cit != namedMatCache.end()) {
                rmId = cit->second;
            } else {
                for (const auto &md : config_.materials) {
                    if (md.name == *matName) {
                        RenderMaterial rm;
                        rm.id = result.scene.nextMaterialId();
                        rm.name = md.name;
                        rm.baseColorFactor = glm::vec4{md.baseColor.r, md.baseColor.g,
                                                       md.baseColor.b, md.baseColor.a};
                        rm.metallicFactor = md.metallic;
                        rm.roughnessFactor = md.roughness;
                        rm.emissiveFactor = glm::vec3{md.emissive.r, md.emissive.g, md.emissive.b};
                        rm.doubleSided = md.doubleSided;
                        rm.alphaMode = md.alphaMode == AlphaMode::Blend  ? "BLEND"
                                       : md.alphaMode == AlphaMode::Mask ? "MASK"
                                                                         : "OPAQUE";
                        rm.alphaCutoff = md.alphaCutoff;
                        rm.ior = md.ior;
                        rm.transmissionFactor = md.transmission;
                        rm.clearcoatFactor = md.clearcoat;
                        rm.clearcoatRoughnessFactor = md.clearcoatRoughness;
                        rm.anisotropyStrength = md.anisotropy;
                        rm.anisotropyRotation = md.anisotropyRotation;
                        rm.specularFactor = md.specularFactor;
                        if (md.specularColor.has_value()) {
                            rm.specularColorFactor = glm::vec3{
                                md.specularColor->r, md.specularColor->g, md.specularColor->b};
                        }
                        rmId = rm.id;
                        result.scene.materials[rmId] = std::move(rm);
                        namedMatCache[md.name] = rmId;
                        break;
                    }
                }
            }
        }
        if (!result.scene.materials.contains(rmId)) {
            if (!defaultMatCache.contains(srcMatId)) {
                auto dm = makeDefaultMaterial(result.scene, srcMat);
                defaultMatCache[srcMatId] = dm.id;
                result.scene.materials[dm.id] = std::move(dm);
            }
            rmId = defaultMatCache.at(srcMatId);
        }
        return rmId;
    };

    // Cache: logVolId → vector<MeshBinding> for merge_descendants reuse.
    // Identical subtrees (same TGeoVolume) produce identical merged meshes.
    std::unordered_map<SemanticLogVolId, std::vector<MeshBinding>> mergeCache;

    // Map SemanticNodeId → RenderNodeId for parent-linking after creation.
    std::unordered_map<SemanticNodeId, RenderNodeId> nodeMap;

    // BFS order ensures parents are processed before children.
    std::queue<SemanticNodeId> q;
    if (!scene.nodes.contains(scene.rootId)) {
        return result;
    }
    q.push(scene.rootId);

    while (!q.empty()) {
        const auto semId = q.front();
        q.pop();
        if (!scene.nodes.contains(semId)) {
            continue;
        }

        const SemanticNode &semNode = scene.nodes.at(semId);

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

        // ── Resolve extras from unified rules ──────────────────────────────────
        {
            NodeView ev = makeNodeView(semNode);
            rn.extras = resolveExtras(config_.rules, ev);
        }

        nodeMap[semId] = rnId;
        if (!semNode.parentId.has_value()) {
            result.scene.rootId = rnId;
        }

        // ── Resolve tessellation rule ──────────────────────────────────────────
        NodeView nv = makeNodeView(semNode);
        const Rule::Tessellation &rule = resolveTessellation(config_.rules, nv);
        TessellationParams params;
        params.maxSegmentsCircle = rule.maxSegmentsCircle;

        // skip_geometry: add the node to the render tree as a structural node but produce no mesh.
        if (rule.skipGeometry) {
            result.scene.nodes[rnId] = rn;
            for (const auto childId : semNode.children) {
                q.push(childId);
            }
            continue;
        }

        // merge_descendants: tessellate all descendants, group by material, and
        // combine into per-material meshes on this node. Children are not added
        // to the render tree.
        if (rule.mergeDescendants) {
            // Check the merge cache — identical logVols produce identical merges.
            auto mcIt = mergeCache.find(semNode.logVolId);
            if (mcIt != mergeCache.end()) {
                rn.meshBindings = mcIt->second;
                result.scene.nodes[rnId] = std::move(rn);
                continue;
            }

            const glm::dmat4 invWorld = glm::affineInverse(semNode.worldTransform);

            // Per-material accumulation buffers.
            struct MatGroup {
                std::vector<Vertex> verts;
                std::vector<uint32_t> indices;
            };
            std::map<RenderMaterialId, MatGroup> groups;

            // BFS descendants
            std::queue<SemanticNodeId> descQ;
            for (const auto childId : semNode.children) {
                descQ.push(childId);
            }
            while (!descQ.empty()) {
                const auto descId = descQ.front();
                descQ.pop();
                if (!scene.nodes.contains(descId)) {
                    continue;
                }
                const SemanticNode &descNode = scene.nodes.at(descId);

                for (const auto gcId : descNode.children) {
                    descQ.push(gcId);
                }

                if (!scene.logVols.contains(descNode.logVolId)) {
                    continue;
                }
                const SemanticLogicalVolume &descLv = scene.logVols.at(descNode.logVolId);
                if (!scene.shapes.contains(descLv.shapeId)) {
                    continue;
                }

                // Tessellate (using cache)
                const SemanticShape &descShape = scene.shapes.at(descLv.shapeId);
                const bool descIsBoolean =
                    std::holds_alternative<BooleanUnion>(descShape.data) ||
                    std::holds_alternative<BooleanIntersection>(descShape.data) ||
                    std::holds_alternative<BooleanSubtraction>(descShape.data);
                auto &descSegMap = meshCache[descLv.shapeId];
                MeshAssetId descMid;
                bool isBBoxProxy = false;
                if (!descSegMap.contains(params.maxSegmentsCircle)) {
                    TessellationOutput tessOut;
                    if (descIsBoolean) {
                        tessOut = tessellateBooleanShape(descShape.data, scene, tess, params);
                    } else {
                        tessOut = tess.tessellate(descShape.data, params);
                    }
                    result.diags.append(tessOut.diags);
                    if (descIsBoolean && tessOut.vertices.empty()) {
                        // Boolean tessellation failed — apply fallback.
                        switch (rule.fallback) {
                        case BooleanFallback::Fail:
                            result.diags.error(
                                codes::kErrTessBooleanFail,
                                std::format("boolean shape on node '{}' cannot be tessellated "
                                            "(fallback=fail)",
                                            descNode.name),
                                descNode.name);
                            return result;
                        case BooleanFallback::BBox:
                            tessOut = makeBBoxProxy(descShape.data, scene, tess, params);
                            result.diags.append(tessOut.diags);
                            isBBoxProxy = true;
                            break;
                        case BooleanFallback::Skip:
                        default:
                            continue;
                        }
                    }
                    if (tessOut.vertices.empty()) {
                        continue;
                    }
                    MeshAsset ma;
                    ma.id = result.scene.nextMeshId();
                    ma.name = descLv.name;
                    ma.vertices = std::move(tessOut.vertices);
                    ma.indices = std::move(tessOut.indices);
                    ma.provenance.sourceSystem = "tessellation_pass";
                    ma.provenance.sourceName = descLv.name;
                    descMid = ma.id;
                    result.scene.meshAssets[descMid] = std::move(ma);
                    descSegMap[params.maxSegmentsCircle] = descMid;
                } else {
                    descMid = descSegMap.at(params.maxSegmentsCircle);
                }

                if (!result.scene.meshAssets.contains(descMid)) {
                    continue;
                }
                const MeshAsset &srcMesh = result.scene.meshAssets.at(descMid);
                if (srcMesh.vertices.empty()) {
                    continue;
                }

                // Resolve material — use red proxy for bbox fallbacks.
                RenderMaterialId rmId = isBBoxProxy
                                            ? getBBoxProxyMaterial()
                                            : resolveRenderMaterial(descLv.materialId, descNode);

                // Transform vertices into merge node's local frame
                const glm::mat4 toLocal = glm::mat4(invWorld * descNode.worldTransform);
                const glm::mat3 normalMat =
                    glm::mat3(glm::transpose(glm::inverse(glm::mat3(toLocal))));

                auto &grp = groups[rmId];
                const auto idxBase = static_cast<uint32_t>(grp.verts.size());
                for (const auto &v : srcMesh.vertices) {
                    Vertex tv;
                    tv.position = glm::vec3(toLocal * glm::vec4(v.position, 1.0f));
                    tv.normal = glm::normalize(normalMat * v.normal);
                    grp.verts.push_back(tv);
                }
                for (const auto idx : srcMesh.indices) {
                    grp.indices.push_back(idx + idxBase);
                }
            }

            if (groups.empty()) {
                result.diags.warn(
                    codes::kWarnTessMergeEmpty,
                    std::format("merge_descendants on '{}' produced no geometry — node has no "
                                "tessellatable descendants (did selection remove them?)",
                                semNode.name),
                    semNode.name);
            }

            // Create one MeshAsset per material group → one MeshBinding each.
            for (auto &[rmId, grp] : groups) {
                MeshAssetId mid = result.scene.nextMeshId();
                MeshAsset ma;
                ma.id = mid;
                ma.name = semNode.name + "_merged";
                ma.vertices = std::move(grp.verts);
                ma.indices = std::move(grp.indices);
                ma.provenance.sourceSystem = "tessellation_pass";
                ma.provenance.sourceName = semNode.name;
                result.scene.meshAssets[mid] = std::move(ma);
                rn.meshBindings.push_back({mid, rmId});
            }

            mergeCache[semNode.logVolId] = rn.meshBindings;

            // Don't enqueue children — they've been consumed by the merge.
            result.scene.nodes[rnId] = std::move(rn);
            continue;
        }

        // ── Resolve shape ──────────────────────────────────────────────────────
        if (!scene.logVols.contains(semNode.logVolId)) {
            result.scene.nodes[rnId] = rn;
            for (const auto childId : semNode.children) {
                q.push(childId);
            }
            continue;
        }
        const SemanticLogicalVolume &lv = scene.logVols.at(semNode.logVolId);
        if (!scene.shapes.contains(lv.shapeId)) {
            result.scene.nodes[rnId] = rn;
            for (const auto childId : semNode.children) {
                q.push(childId);
            }
            continue;
        }
        const SemanticShape &shape = scene.shapes.at(lv.shapeId);
        const bool isBoolean = std::holds_alternative<BooleanUnion>(shape.data) ||
                               std::holds_alternative<BooleanIntersection>(shape.data) ||
                               std::holds_alternative<BooleanSubtraction>(shape.data);

        // ── Boolean handling ───────────────────────────────────────────────────
        if (isBoolean) {
            // Check the mesh cache first — same shapeId + segments → same mesh.
            auto &boolSegMap = meshCache[lv.shapeId];
            MeshAssetId boolMid;
            bool boolCacheHit = boolSegMap.contains(params.maxSegmentsCircle);
            if (boolCacheHit) {
                boolMid = boolSegMap.at(params.maxSegmentsCircle);
            } else {
                auto boolOut = tessellateBooleanShape(shape.data, scene, tess, params);
                result.diags.append(boolOut.diags);
                if (!boolOut.vertices.empty()) {
                    boolMid = result.scene.nextMeshId();
                    MeshAsset ma;
                    ma.id = boolMid;
                    ma.name = lv.name + "_boolean";
                    ma.vertices = std::move(boolOut.vertices);
                    ma.indices = std::move(boolOut.indices);
                    ma.provenance.sourceSystem = "tessellation_pass/manifold";
                    ma.provenance.sourceName = lv.name;
                    result.scene.meshAssets[boolMid] = std::move(ma);
                    boolSegMap[params.maxSegmentsCircle] = boolMid;
                    boolCacheHit = true;
                }
            }
            if (boolCacheHit) {
                RenderMaterialId rmId = resolveRenderMaterial(lv.materialId, semNode);
                rn.meshBindings.push_back({boolMid, rmId});
                result.scene.nodes[rnId] = std::move(rn);
                for (const auto childId : semNode.children) {
                    q.push(childId);
                }
                continue;
            }
            // Manifold failed or produced empty output — fall through to fallback.
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
                auto bboxOut = makeBBoxProxy(shape.data, scene, tess, params);
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

                rn.meshBindings.push_back({mid, getBBoxProxyMaterial()});
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
            for (const auto childId : semNode.children) {
                q.push(childId);
            }
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
        RenderMaterialId rmId = resolveRenderMaterial(lv.materialId, semNode);

        // Only bind mesh if tessellation produced geometry (UnknownShape may be empty)
        if (result.scene.meshAssets.contains(mid) &&
            !result.scene.meshAssets.at(mid).vertices.empty()) {
            rn.meshBindings.push_back({mid, rmId});
        }

        result.scene.nodes[rnId] = std::move(rn);
        for (const auto childId : semNode.children) {
            q.push(childId);
        }
    }

    std::println(stderr, "Tessellation stats:");
    std::println(stderr, "  Render nodes:     {}", result.scene.nodes.size());
    std::println(stderr, "  Unique meshes:    {}", result.scene.meshAssets.size());
    std::println(stderr, "  Unique materials: {}", result.scene.materials.size());
    std::println(stderr, "  Mesh cache entries (shapes with tessellation): {}", meshCache.size());
    return result;
}

} // namespace nodehammer
