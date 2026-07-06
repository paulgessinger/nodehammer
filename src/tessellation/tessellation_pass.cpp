#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/selection/predicate.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>
#include <nodehammer/tessellation/tessellation_job.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <nodehammer/tessellation/boolean_tessellator.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <print>
#include <queue>

namespace nodehammer {

namespace {

std::size_t hashCombine(std::size_t seed, std::size_t h) {
    return seed ^ (h + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (seed << 6) + (seed >> 2));
}

std::size_t hashDouble(double d) { return std::hash<uint64_t>{}(std::bit_cast<uint64_t>(d)); }

std::size_t hashMatrix(const glm::dmat4 &m) {
    std::size_t h = 0;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            h = hashCombine(h, hashDouble(m[c][r]));
        }
    }
    return h;
}

bool matrixEqual(const glm::dmat4 &a, const glm::dmat4 &b) {
    return std::memcmp(&a, &b, sizeof(glm::dmat4)) == 0;
}

bool matrixLess(const glm::dmat4 &a, const glm::dmat4 &b) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            const auto av = std::bit_cast<uint64_t>(a[c][r]);
            const auto bv = std::bit_cast<uint64_t>(b[c][r]);
            if (av != bv) {
                return av < bv;
            }
        }
    }
    return false;
}

// A Rule paired with its once-compiled match predicate. Compiling is non-trivial
// (recursive closure construction), so doing it per node × per rule × per resolver
// shows up prominently in profiles — we compile once per lower() call instead.
struct CompiledRule {
    const Rule *rule{nullptr};
    std::optional<Predicate> match; // nullopt ⇔ rule->match is nullopt (match-all)
};

std::vector<CompiledRule> compileRulePredicates(const std::vector<Rule> &rules) {
    std::vector<CompiledRule> out;
    out.reserve(rules.size());
    for (const auto &r : rules) {
        CompiledRule cr;
        cr.rule = &r;
        if (r.match.has_value()) {
            cr.match = compilePredicate(*r.match);
        }
        out.push_back(std::move(cr));
    }
    return out;
}

// Does a rule's match predicate apply to the given node?
bool ruleMatches(const CompiledRule &cr, const NodeView &view) {
    if (cr.match.has_value()) {
        return (*cr.match)(view);
    }
    return true; // no predicate → matches everything
}

// Resolved tessellation settings with all fields filled in (no optionals).
struct ResolvedTessellation {
    bool skipGeometry{false};
    bool mergeDescendants{false};
    // See Rule::Tessellation::dropCoincidentFaces (config_ast.hpp) — only acted on
    // when mergeDescendants is also true; the coincident-face removal pass
    // runs on the merged group, not on individual descendants.
    bool dropCoincidentFaces{false};
    int maxSegmentsCircle{64};
    BooleanFallback fallback{BooleanFallback::Skip};
};

struct MergeDescendantSignature {
    SemanticShapeId shapeId;
    SemanticMaterialId sourceMaterialId;
    std::string materialKey;
    glm::dmat4 toMergeLocal{1.0};
    int maxSegmentsCircle{64};

    bool operator==(const MergeDescendantSignature &o) const {
        return shapeId == o.shapeId && sourceMaterialId == o.sourceMaterialId &&
               materialKey == o.materialKey && matrixEqual(toMergeLocal, o.toMergeLocal) &&
               maxSegmentsCircle == o.maxSegmentsCircle;
    }
};

struct MergeCacheKey {
    BooleanFallback fallback{BooleanFallback::Skip};
    // Resolved on the merge_descendants node itself (not its descendants): it
    // changes the merged mesh — interior faces are dropped only when set — so a
    // reused prototype must match on it or an enabled node could keep its
    // interior faces (or a disabled one lose them) depending on traversal order.
    bool dropCoincidentFaces{false};
    std::vector<MergeDescendantSignature> descendants;

    bool operator==(const MergeCacheKey &) const = default;
};

struct MergeCacheKeyHash {
    std::size_t operator()(const MergeCacheKey &k) const {
        std::size_t h = std::hash<int>{}(static_cast<int>(k.fallback));
        h = hashCombine(h, std::hash<bool>{}(k.dropCoincidentFaces));
        h = hashCombine(h, std::hash<std::size_t>{}(k.descendants.size()));
        for (const auto &d : k.descendants) {
            h = hashCombine(h, std::hash<uint64_t>{}(d.shapeId.value));
            h = hashCombine(h, std::hash<uint64_t>{}(d.sourceMaterialId.value));
            h = hashCombine(h, std::hash<std::string>{}(d.materialKey));
            h = hashCombine(h, hashMatrix(d.toMergeLocal));
            h = hashCombine(h, std::hash<int>{}(d.maxSegmentsCircle));
        }
        return h;
    }
};

struct MergeDescendant {
    SemanticNodeId nodeId;
    glm::dmat4 toMergeLocal{1.0};
    int maxSegmentsCircle{64};
};

struct PrototypeDescendantSignature {
    SemanticShapeId shapeId;
    SemanticMaterialId sourceMaterialId;
    glm::dmat4 toMergeLocal{1.0};
};

bool collectPrototypeLeafDescendants(const SemanticScene &scene, SemanticLogVolId rootLv,
                                     std::vector<PrototypeDescendantSignature> &out) {
    if (!scene.logVols.contains(rootLv)) {
        return false;
    }
    const auto &root = scene.logVols.at(rootLv);
    if (root.daughters.empty()) {
        return false;
    }

    std::queue<std::pair<SemanticLogVolId, glm::dmat4>> q;
    for (const auto &daughter : root.daughters) {
        q.push({daughter.logVolId, daughter.localTransform});
    }

    while (!q.empty()) {
        const auto [lvId, toRootLocal] = q.front();
        q.pop();
        if (!scene.logVols.contains(lvId)) {
            return false;
        }
        const auto &lv = scene.logVols.at(lvId);
        if (lv.daughters.empty()) {
            out.push_back({lv.shapeId, lv.materialId, toRootLocal});
        } else {
            for (const auto &daughter : lv.daughters) {
                q.push({daughter.logVolId, toRootLocal * daughter.localTransform});
            }
        }
    }

    return true;
}

bool prototypeDescendantLess(const PrototypeDescendantSignature &a,
                             const PrototypeDescendantSignature &b) {
    if (a.shapeId.value != b.shapeId.value) {
        return a.shapeId.value < b.shapeId.value;
    }
    if (a.sourceMaterialId.value != b.sourceMaterialId.value) {
        return a.sourceMaterialId.value < b.sourceMaterialId.value;
    }
    return matrixLess(a.toMergeLocal, b.toMergeLocal);
}

bool mergeDescendantLess(const MergeDescendantSignature &a, const MergeDescendantSignature &b) {
    if (a.shapeId.value != b.shapeId.value) {
        return a.shapeId.value < b.shapeId.value;
    }
    if (a.sourceMaterialId.value != b.sourceMaterialId.value) {
        return a.sourceMaterialId.value < b.sourceMaterialId.value;
    }
    if (a.materialKey != b.materialKey) {
        return a.materialKey < b.materialKey;
    }
    if (a.maxSegmentsCircle != b.maxSegmentsCircle) {
        return a.maxSegmentsCircle < b.maxSegmentsCircle;
    }
    return matrixLess(a.toMergeLocal, b.toMergeLocal);
}

void sortMergeDescendants(std::vector<MergeDescendantSignature> &descendants) {
    std::sort(descendants.begin(), descendants.end(), mergeDescendantLess);
}

using ShapeMaterialKey = std::pair<uint64_t, uint64_t>;

bool tryUsePrototypeMergeKey(const SemanticScene &scene, SemanticLogVolId rootLv,
                             MergeCacheKey &mergeKey) {
    std::vector<PrototypeDescendantSignature> prototypeDescendants;
    if (!collectPrototypeLeafDescendants(scene, rootLv, prototypeDescendants) ||
        prototypeDescendants.size() != mergeKey.descendants.size()) {
        return false;
    }

    struct SelectedMaterialUse {
        std::size_t count{0};
        std::string materialKey;
        int maxSegmentsCircle{64};
        bool hasMaterialKey{false};
        bool ambiguous{false};
    };

    std::map<ShapeMaterialKey, SelectedMaterialUse> selectedUses;
    for (const auto &selected : mergeKey.descendants) {
        auto &use = selectedUses[{selected.shapeId.value, selected.sourceMaterialId.value}];
        ++use.count;
        if (!use.hasMaterialKey) {
            use.materialKey = selected.materialKey;
            use.maxSegmentsCircle = selected.maxSegmentsCircle;
            use.hasMaterialKey = true;
        } else if (use.materialKey != selected.materialKey ||
                   use.maxSegmentsCircle != selected.maxSegmentsCircle) {
            use.ambiguous = true;
        }
    }

    for (const auto &[_, use] : selectedUses) {
        if (use.ambiguous) {
            return false;
        }
    }

    std::sort(prototypeDescendants.begin(), prototypeDescendants.end(), prototypeDescendantLess);

    std::vector<MergeDescendantSignature> prototypeKey;
    prototypeKey.reserve(prototypeDescendants.size());
    for (const auto &proto : prototypeDescendants) {
        auto it = selectedUses.find({proto.shapeId.value, proto.sourceMaterialId.value});
        if (it == selectedUses.end() || it->second.count == 0) {
            return false;
        }
        --it->second.count;
        prototypeKey.push_back({proto.shapeId, proto.sourceMaterialId, it->second.materialKey,
                                proto.toMergeLocal, it->second.maxSegmentsCircle});
    }

    for (const auto &[_, use] : selectedUses) {
        if (use.count != 0) {
            return false;
        }
    }

    sortMergeDescendants(prototypeKey);
    mergeKey.descendants = std::move(prototypeKey);
    return true;
}

// Merge tessellation settings from all matching rules (last match wins per
// field), then apply config defaults, then hardcoded defaults for anything
// still unset.
ResolvedTessellation resolveTessellation(const std::vector<CompiledRule> &rules,
                                         const Rule::Tessellation &defaults, const NodeView &view) {
    Rule::Tessellation merged;
    for (const auto &cr : rules) {
        if (!cr.rule->tessellation.has_value()) {
            continue;
        }
        if (!ruleMatches(cr, view)) {
            continue;
        }
        const auto &t = *cr.rule->tessellation;
        if (t.skipGeometry.has_value()) {
            merged.skipGeometry = *t.skipGeometry;
        }
        if (t.mergeDescendants.has_value()) {
            merged.mergeDescendants = *t.mergeDescendants;
        }
        if (t.dropCoincidentFaces.has_value()) {
            merged.dropCoincidentFaces = *t.dropCoincidentFaces;
        }
        if (t.maxSegmentsCircle.has_value()) {
            merged.maxSegmentsCircle = *t.maxSegmentsCircle;
        }
        if (t.fallback.has_value()) {
            merged.fallback = *t.fallback;
        }
    }
    // Config defaults override hardcoded defaults but not rule-matched values.
    return ResolvedTessellation{
        .skipGeometry = merged.skipGeometry.value_or(defaults.skipGeometry.value_or(false)),
        .mergeDescendants =
            merged.mergeDescendants.value_or(defaults.mergeDescendants.value_or(false)),
        .dropCoincidentFaces =
            merged.dropCoincidentFaces.value_or(defaults.dropCoincidentFaces.value_or(false)),
        .maxSegmentsCircle =
            merged.maxSegmentsCircle.value_or(defaults.maxSegmentsCircle.value_or(64)),
        .fallback = merged.fallback.value_or(defaults.fallback.value_or(BooleanFallback::Skip)),
    };
}

// Return the material name from the last matching rule that has one, or nullptr.
// This is consistent with tessellation resolution (last-match-wins per field)
// and means later includes / parent configs can override earlier ones.
const std::string *resolveMaterial(const std::vector<CompiledRule> &rules, const NodeView &view) {
    const std::string *result = nullptr;
    for (const auto &cr : rules) {
        if (!cr.rule->material.has_value()) {
            continue;
        }
        if (ruleMatches(cr, view)) {
            result = &*cr.rule->material;
        }
    }
    return result;
}

// ── Coincident interior-face removal (drop_coincident_faces) ──────────────────────
//
// A sampling calorimeter is stacked absorber/scintillator slabs, touching
// face-to-face. Every internal interface between two touching OPAQUE slabs is
// a pair of geometrically coincident, oppositely-wound triangles that can
// never be seen (they face directly into each other, buried inside the
// stack). The rasterizer still pays full price for them — this is the
// dominant cost of the calo overdraw (see the module-level background in the
// task that introduced this pass). Stripping exactly those pairs collapses
// depth complexity from ~2N to ~2 with no visible change, as long as nothing
// ever exposes the interior (a boolean cut would — that's a phase-2 concern;
// this pass runs before any such cut is baked, so it is unaware of it).
//
// IMPORTANT / scope: this is only correct for fully-opaque geometry. A
// translucent stack *can* show its interior faces through the material above
// it, so removing them there would be visibly wrong. Phase 1 does not gate on
// opacity in code — the flag is opt-in per rule, and it is the config
// author's job to only set drop_coincident_faces=true on stacks that are actually
// opaque (e.g. the calo staves). See config_ast.hpp's Tessellation struct.

// Absolute snap tolerance (in merge-local units, i.e. after the per-descendant
// transform at the `toLocal` multiply above) used to quantize vertex
// positions before comparing triangles for coincidence. This absorbs the tiny
// float roundoff introduced by transforming each descendant's vertices into
// the merge node's local frame — two slabs whose CAD-authored surfaces are
// bit-for-bit identical will not, in general, transform to bit-identical
// floats. ODD calo slabs are O(1-100 mm) in local units, so 1e-4 (0.1 micron)
// comfortably separates "same point after roundoff" from "different point"
// without merging distinct nearby vertices. Tune here if a future geometry's
// units or roundoff characteristics differ.
constexpr float kCoincidentSnapTolerance = 1e-4f;

// Quantizes a single float to the snap grid, rounding to the nearest grid line
// so that two floats within tolerance of each other always snap to the same
// integer.
inline int64_t snapCoord(float v) {
    return static_cast<int64_t>(
        std::llround(static_cast<double>(v) / static_cast<double>(kCoincidentSnapTolerance)));
}

// Quantized 3D point used as (part of) the coincident-triangle key.
struct SnappedPoint {
    int64_t x, y, z;
    bool operator==(const SnappedPoint &) const = default;
    bool operator<(const SnappedPoint &o) const {
        if (x != o.x)
            return x < o.x;
        if (y != o.y)
            return y < o.y;
        return z < o.z;
    }
};

SnappedPoint snapPoint(const glm::vec3 &p) {
    return {snapCoord(p.x), snapCoord(p.y), snapCoord(p.z)};
}

// A back-reference to one triangle: which material group's index buffer, and
// the index of its first corner (indices[triBase], [triBase+1], [triBase+2]).
struct TriRef {
    RenderMaterialId group;
    size_t triBase;
};

// Order-independent key for a triangle's 3 quantized corners — two triangles
// sharing all 3 points (in any order/winding) share a key. Coincident,
// opposite-wound interior faces always produce identical keys this way.
struct TriKey {
    std::array<SnappedPoint, 3> pts; // sorted ascending
    bool operator==(const TriKey &) const = default;
};

struct TriKeyHash {
    size_t operator()(const TriKey &k) const noexcept {
        size_t h = 0;
        for (const auto &p : k.pts) {
            h = hashCombine(h, std::hash<int64_t>{}(p.x));
            h = hashCombine(h, std::hash<int64_t>{}(p.y));
            h = hashCombine(h, std::hash<int64_t>{}(p.z));
        }
        return h;
    }
};

// Label used to group drop_coincident_faces candidates by structure in the NH0509
// suggestion: the node's original source path minus its own final segment (i.e.
// the parent structure that holds the stack, e.g. ".../ECalBarrel"), falling
// back to the node name when no path is recorded.
inline std::string candidateStructureLabel(const SemanticNode &node) {
    const std::string &p = node.originalPath;
    if (!p.empty()) {
        const auto slash = p.find_last_of('/');
        if (slash != std::string::npos && slash > 0) {
            return p.substr(0, slash);
        }
        return p;
    }
    return node.name.empty() ? std::string{"<unnamed>"} : node.name;
}

// Removes exact-coincident, opposite-wound triangle pairs across ALL material
// groups of a merge_descendants result jointly (the interior interfaces are
// *between* alternating materials, e.g. absorber → scintillator, so a
// per-group search would never find them).
//
// Conservative removal rule: a key is only acted on when EXACTLY two
// triangles share it and their geometric normals oppose (dot < -0.9). Keys
// with a single triangle (an exterior face — nothing to pair with) or 3+
// triangles (a degenerate/non-manifold coincidence we don't want to guess
// about) are left untouched. This trades a bit of missed overdraw reduction
// at weird junctions for never silently deleting a face that was actually
// visible.
//
// Mutates each group's `indices` in place, dropping removed triangles'
// index triples. Vertex compaction is intentionally skipped — the now-
// unreferenced vertices are harmless dead weight for an indexed draw, and
// compacting them would need a remap pass that isn't worth the complexity
// here. Returns the number of triangles removed (always even — pairs).
// Templated on the group-map type (rather than named on MatGroup directly)
// because MatGroup is a local struct defined inside
// tessellateMergeDescendants — a template lets this free function operate on
// it without exposing that type at namespace scope.
// A merge_descendants node is flagged as a drop_coincident_faces *candidate* only
// when count-only detection finds at least this many removable interior faces.
// Kept well above the handful of faces two incidentally-touching solids share,
// so the suggestion fires on genuine stacked-slab structures (sampling calos)
// rather than on every pair of adjacent boxes.
inline constexpr size_t kCoincidentCandidateMinFaces = 32;

// When `apply` is false the index buffers are left untouched and the function
// only *counts* how many faces it would remove — used to flag merge_descendants
// nodes that are good drop_coincident_faces candidates without mutating them.
template <typename MatGroupMap>
size_t removeCoincidentInteriorFaces(MatGroupMap &groups, bool apply = true) {
    // Pass 1: flatten every triangle in every group into a lookup keyed by its
    // quantized, order-independent corner set.
    ankerl::unordered_dense::map<TriKey, std::vector<TriRef>, TriKeyHash> byKey;

    for (auto &[rmId, grp] : groups) {
        const size_t triCount = grp.indices.size() / 3;
        for (size_t t = 0; t < triCount; ++t) {
            const size_t base = t * 3;
            const glm::vec3 &p0 = grp.verts[grp.indices[base + 0]].position;
            const glm::vec3 &p1 = grp.verts[grp.indices[base + 1]].position;
            const glm::vec3 &p2 = grp.verts[grp.indices[base + 2]].position;

            std::array<SnappedPoint, 3> pts{snapPoint(p0), snapPoint(p1), snapPoint(p2)};
            std::sort(pts.begin(), pts.end());
            TriKey key{pts};

            byKey[key].push_back(TriRef{rmId, base});
        }
    }

    // Pass 2: for keys with exactly 2 triangles and opposing geometric
    // normals, mark both for removal. Collect (group, triBase) pairs first —
    // we mutate index buffers afterwards so the flatten pass above stays
    // valid throughout.
    ankerl::unordered_dense::map<RenderMaterialId, ankerl::unordered_dense::set<size_t>>
        toRemoveByGroup;

    auto geometricNormal = [&groups](const TriRef &ref) -> glm::vec3 {
        auto &grp = groups.at(ref.group);
        const glm::vec3 &p0 = grp.verts[grp.indices[ref.triBase + 0]].position;
        const glm::vec3 &p1 = grp.verts[grp.indices[ref.triBase + 1]].position;
        const glm::vec3 &p2 = grp.verts[grp.indices[ref.triBase + 2]].position;
        return glm::normalize(glm::cross(p1 - p0, p2 - p0));
    };

    size_t removedCount = 0;
    for (const auto &[key, refs] : byKey) {
        if (refs.size() != 2) {
            // 1 → genuine exterior face; 3+ → non-manifold/degenerate overlap
            // we deliberately don't touch (see function comment).
            continue;
        }
        const glm::vec3 n0 = geometricNormal(refs[0]);
        const glm::vec3 n1 = geometricNormal(refs[1]);
        if (glm::dot(n0, n1) >= -0.9f) {
            // Same-facing coincident triangles aren't an interior seam (could
            // be a degenerate duplicate) — leave them for now, conservative.
            continue;
        }
        toRemoveByGroup[refs[0].group].insert(refs[0].triBase);
        toRemoveByGroup[refs[1].group].insert(refs[1].triBase);
        removedCount += 2;
    }

    // Count-only (candidate detection): report the tally without mutating.
    if (!apply) {
        return removedCount;
    }

    // Pass 3: rebuild each group's index buffer, skipping removed triangles.
    for (auto &[rmId, grp] : groups) {
        auto it = toRemoveByGroup.find(rmId);
        if (it == toRemoveByGroup.end()) {
            continue; // nothing removed from this group
        }
        const auto &removedBases = it->second;
        std::vector<uint32_t> kept;
        kept.reserve(grp.indices.size());
        const size_t triCount = grp.indices.size() / 3;
        for (size_t t = 0; t < triCount; ++t) {
            const size_t base = t * 3;
            if (removedBases.contains(base)) {
                continue;
            }
            kept.push_back(grp.indices[base + 0]);
            kept.push_back(grp.indices[base + 1]);
            kept.push_back(grp.indices[base + 2]);
        }
        grp.indices = std::move(kept);
    }

    return removedCount;
}

// Resolve extras from the first matching rule that has them, falling back to
// config-level defaults.
RenderExtrasMap resolveExtras(const std::vector<CompiledRule> &rules,
                              const std::optional<ExtrasMap> &defaults, const NodeView &view) {
    for (const auto &cr : rules) {
        if (!cr.rule->extras.has_value()) {
            continue;
        }
        if (ruleMatches(cr, view)) {
            return *cr.rule->extras;
        }
    }
    if (defaults.has_value()) {
        return *defaults;
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
        if constexpr (is_boolean_shape_v<T>) {
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
                if constexpr (is_boolean_shape_v<T>) {
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

// ── TessellationJob ──────────────────────────────────────────────────────────
//
// The body of the BFS walk lives on `Impl::stepOneNode`, mirroring the
// inline implementation that used to live in `TessellationPass::lower`.
// `lower` is now a thin run-to-completion driver around a `TessellationJob`.

struct TessellationJob::Impl {
    const NHConfig *config{nullptr};
    const SemanticScene *scene{nullptr};
    /// True when the scene carries the wedge-cut marker logVol, i.e. an
    /// azimuthal cut was applied upstream. An empty merge_descendants result is
    /// then an expected consequence of the cut, not a selection/config error.
    bool wedgeCutApplied{false};

    TessellationPassResult result;

    std::vector<CompiledRule> compiledRules;
    PrimitiveTessellator tess;

    std::optional<RenderMaterialId> bboxProxyMatId;

    ankerl::unordered_dense::map<SemanticMaterialId, RenderMaterialId> defaultMatCache;
    ankerl::unordered_dense::map<std::string, RenderMaterialId> namedMatCache;
    ankerl::unordered_dense::map<SemanticShapeId, ankerl::unordered_dense::map<int, MeshAssetId>>
        meshCache;
    ankerl::unordered_dense::map<MergeCacheKey, std::vector<MeshBinding>, MergeCacheKeyHash>
        mergeCache;
    ankerl::unordered_dense::map<SemanticNodeId, RenderNodeId> nodeMap;

    std::queue<SemanticNodeId> q;

    bool started{false};
    bool done{false};
    // Counters are atomic so the SceneBuildJob's native worker thread can
    // bump `processedNodes` while the main thread reads it for the UI bar
    // (relaxed ordering — the bar tolerates a frame of staleness). Set
    // once at start(), then mutated only by the BFS, then read after the
    // worker joins.
    std::atomic<size_t> totalNodes{0};
    std::atomic<size_t> processedNodes{0};
    // Running total of triangles dropped by removeCoincidentInteriorFaces()
    // across every drop_coincident_faces group in this job — surfaced in the
    // take()-time stats print so an A/B config toggle is verifiable without
    // a debugger (see tessellation_pass.cpp's merge-finalization step).
    std::atomic<size_t> coincidentFacesRemoved{0};
    // Number of merge_descendants nodes that had at least one coincident face
    // removed — paired with coincidentFacesRemoved so take() can emit a single
    // aggregate diagnostic instead of one toast per node (there are 100+ calo
    // staves; per-node info diags flood the viewer's toast area).
    std::atomic<size_t> coincidentNodesAffected{0};
    // Discoverability: on merge_descendants nodes where drop_coincident_faces is OFF
    // we still run the detection in count-only mode. If a node has a meaningful
    // number of removable interior faces it's a good candidate for the option.
    // Candidates are grouped by parent structure (the node's path minus its own
    // segment) so take() can name *which* structures in a single suggestion
    // diagnostic — value is {instance count, removable faces × instances}. Plain
    // (not atomic): only written by the single-threaded BFS and read in take()
    // after the worker joins, exactly like result.diags.
    //
    // Merged prototypes are deduplicated by the merge cache: detection runs once
    // (on the first, cache-miss instance) but the reduced mesh is reused by every
    // other instance. To report the true footprint we remember each candidate
    // prototype's removable-face count keyed by its merge key, and on each later
    // cache hit attribute one more instance (and its faces) to the structure that
    // hit actually lives in — so e.g. both barrel layers sharing one stave
    // prototype are each credited their instances, not just the first-seen one.
    std::map<std::string, std::pair<size_t, size_t>> coincidentCandidatesByParent;
    ankerl::unordered_dense::map<MergeCacheKey, size_t, MergeCacheKeyHash> dropCandidateByKey;

    NodeView makeNodeView(const SemanticNode &node) const {
        std::string_view matName;
        if (scene->logVols.contains(node.logVolId)) {
            const auto &lv = scene->logVols.at(node.logVolId);
            if (scene->materials.contains(lv.materialId)) {
                matName = scene->materials.at(lv.materialId).name;
            }
        }
        return {node.name, node.originalPath, matName, node.children.empty(), &node.tags};
    }

    RenderMaterialId getBBoxProxyMaterial() {
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
    }

    RenderMaterialId resolveRenderMaterial(SemanticMaterialId srcMatId, const SemanticNode &node);

    // Process one outer-BFS iteration. Returns false if the queue became
    // empty (no more nodes to process) OR if the pass has already aborted
    // due to an error. After the queue drains, `done` is set.
    //
    // stepOneNode() builds the RenderNode and resolves the tessellation rule,
    // then dispatches the geometry work to one of these branch helpers. Each
    // finalizes `rn` into the render scene and returns the same bool contract
    // as stepOneNode (false only on a fallback=fail abort).
    bool stepOneNode();
    bool tessellateMergeDescendants(const SemanticNode &semNode, RenderNode &rn, RenderNodeId rnId,
                                    const ResolvedTessellation &rule);
    bool tessellateBooleanNode(const SemanticNode &semNode, const SemanticLogicalVolume &lv,
                               const SemanticShape &shape, RenderNode &rn, RenderNodeId rnId,
                               const ResolvedTessellation &rule, const TessellationParams &params);
    bool tessellatePrimitiveNode(const SemanticNode &semNode, const SemanticLogicalVolume &lv,
                                 const SemanticShape &shape, RenderNode &rn, RenderNodeId rnId,
                                 const TessellationParams &params);

    // Count the number of nodes that the outer BFS in `stepOneNode`
    // will actually process. Mirrors that loop's children-skip rules so
    // `processedNodes / totalNodes` reaches 100% at the end:
    //
    //   - merge_descendants nodes consume their entire subtree in one
    //     outer iteration, so we count the merge node but stop walking
    //     into its children.
    //   - all other nodes (skip_geometry, missing logVol/shape, primitive,
    //     boolean) enqueue children, same as the real pass.
    //
    // We do *not* try to predict BooleanFallback::Fail aborts — those
    // stop the pass mid-walk, and accepting a slightly-overshoot total
    // in that error path is fine.
    size_t countEffectiveNodes() const {
        if (scene == nullptr || !scene->nodes.contains(scene->rootId)) {
            return 0;
        }
        size_t n = 0;
        std::queue<SemanticNodeId> qq;
        qq.push(scene->rootId);
        while (!qq.empty()) {
            const auto id = qq.front();
            qq.pop();
            if (!scene->nodes.contains(id)) {
                continue;
            }
            ++n;
            const auto &node = scene->nodes.at(id);
            NodeView nv = makeNodeView(node);
            const auto rule = resolveTessellation(compiledRules, config->tessellationDefaults, nv);
            if (rule.mergeDescendants) {
                continue;
            }
            for (const auto childId : node.children) {
                qq.push(childId);
            }
        }
        return n;
    }
};

RenderMaterialId TessellationJob::Impl::resolveRenderMaterial(SemanticMaterialId srcMatId,
                                                              const SemanticNode &node) {
    const auto &srcMat = scene->materials.at(srcMatId);
    NodeView view = makeNodeView(node);
    const std::string *matName = resolveMaterial(compiledRules, view);
    RenderMaterialId rmId;
    if (matName != nullptr) {
        auto cit = namedMatCache.find(*matName);
        if (cit != namedMatCache.end()) {
            rmId = cit->second;
        } else {
            for (const auto &md : config->materials) {
                if (md.name == *matName) {
                    RenderMaterial rm;
                    rm.id = result.scene.nextMaterialId();
                    rm.name = md.name;
                    rm.baseColorFactor =
                        glm::vec4{md.baseColor.r, md.baseColor.g, md.baseColor.b, md.baseColor.a};
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
                        rm.specularColorFactor = glm::vec3{md.specularColor->r, md.specularColor->g,
                                                           md.specularColor->b};
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
            result.diags.warn(codes::kWarnTessDefaultMaterial,
                              std::format("no material rule matched source material '{}'; "
                                          "using default grey fallback",
                                          srcMat.name),
                              node.name);
        }
        rmId = defaultMatCache.at(srcMatId);
    }
    return rmId;
}

bool TessellationJob::Impl::stepOneNode() {
    if (q.empty()) {
        done = true;
        return false;
    }
    const auto semId = q.front();
    q.pop();
    if (!scene->nodes.contains(semId)) {
        return true;
    }
    processedNodes.fetch_add(1, std::memory_order_relaxed);

    const SemanticNode &semNode = scene->nodes.at(semId);

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
        rn.extras = resolveExtras(compiledRules, config->extrasDefaults, ev);
    }

    nodeMap[semId] = rnId;
    if (!semNode.parentId.has_value()) {
        result.scene.rootId = rnId;
    }

    // ── Resolve tessellation rule ──────────────────────────────────────────
    NodeView nv = makeNodeView(semNode);
    const auto rule = resolveTessellation(compiledRules, config->tessellationDefaults, nv);
    TessellationParams params;
    params.maxSegmentsCircle = rule.maxSegmentsCircle;

    // skip_geometry: add the node to the render tree as a structural node but produce no mesh.
    if (rule.skipGeometry) {
        result.scene.nodes[rnId] = rn;
        for (const auto childId : semNode.children) {
            q.push(childId);
        }
        return true;
    }

    // merge_descendants: tessellate all descendants, group by material, and
    // combine into per-material meshes on this node. Children are not added
    // to the render tree.
    if (rule.mergeDescendants) {
        return tessellateMergeDescendants(semNode, rn, rnId, rule);
    }

    // ── Resolve shape ──────────────────────────────────────────────────────
    if (!scene->logVols.contains(semNode.logVolId)) {
        result.scene.nodes[rnId] = rn;
        for (const auto childId : semNode.children) {
            q.push(childId);
        }
        return true;
    }
    const SemanticLogicalVolume &lv = scene->logVols.at(semNode.logVolId);
    if (!scene->shapes.contains(lv.shapeId)) {
        result.scene.nodes[rnId] = rn;
        for (const auto childId : semNode.children) {
            q.push(childId);
        }
        return true;
    }
    const SemanticShape &shape = scene->shapes.at(lv.shapeId);

    if (isBooleanShape(shape.data)) {
        return tessellateBooleanNode(semNode, lv, shape, rn, rnId, rule, params);
    }
    return tessellatePrimitiveNode(semNode, lv, shape, rn, rnId, params);
}

bool TessellationJob::Impl::tessellateMergeDescendants(const SemanticNode &semNode, RenderNode &rn,
                                                       RenderNodeId rnId,
                                                       const ResolvedTessellation &rule) {
    MergeCacheKey mergeKey;
    mergeKey.fallback = rule.fallback;
    mergeKey.dropCoincidentFaces = rule.dropCoincidentFaces;

    std::vector<MergeDescendant> mergeDescendants;

    // BFS descendants. Accumulate transforms down the selected local hierarchy instead
    // of recomputing inverse(parentWorld) * childWorld; the latter introduces tiny
    // roundoff differences that defeat exact cache reuse for repeated source volumes.
    std::queue<MergeDescendant> collectQ;
    for (const auto childId : semNode.children) {
        if (scene->nodes.contains(childId)) {
            collectQ.push({childId, scene->nodes.at(childId).localTransform});
        }
    }
    while (!collectQ.empty()) {
        const auto mergeDesc = collectQ.front();
        collectQ.pop();
        const auto descId = mergeDesc.nodeId;
        if (!scene->nodes.contains(descId)) {
            continue;
        }
        const SemanticNode &descNode = scene->nodes.at(descId);

        for (const auto gcId : descNode.children) {
            if (scene->nodes.contains(gcId)) {
                collectQ.push(
                    {gcId, mergeDesc.toMergeLocal * scene->nodes.at(gcId).localTransform});
            }
        }

        if (!scene->logVols.contains(descNode.logVolId)) {
            continue;
        }
        const SemanticLogicalVolume &descLv = scene->logVols.at(descNode.logVolId);
        if (!scene->shapes.contains(descLv.shapeId)) {
            continue;
        }

        NodeView descView = makeNodeView(descNode);
        const std::string *matName = resolveMaterial(compiledRules, descView);
        const std::string materialKey = matName != nullptr
                                            ? "config:" + *matName
                                            : std::format("source:{}", descLv.materialId.value);

        const auto descTess =
            resolveTessellation(compiledRules, config->tessellationDefaults, descView);
        const int descSegs = descTess.maxSegmentsCircle;

        mergeKey.descendants.push_back(
            {descLv.shapeId, descLv.materialId, materialKey, mergeDesc.toMergeLocal, descSegs});
        mergeDescendants.push_back({mergeDesc.nodeId, mergeDesc.toMergeLocal, descSegs});
    }

    if (!tryUsePrototypeMergeKey(*scene, semNode.logVolId, mergeKey)) {
        sortMergeDescendants(mergeKey.descendants);
    }

    // Check the merge cache only after the placement-aware key is known.
    auto mcIt = mergeCache.find(mergeKey);
    if (mcIt != mergeCache.end()) {
        // Reused prototype. If it was flagged a drop_coincident_faces candidate
        // on its first (cache-miss) instance, credit this reuse to the structure
        // it actually lives in so the suggestion reflects the real footprint.
        if (auto cand = dropCandidateByKey.find(mergeKey); cand != dropCandidateByKey.end()) {
            auto &agg = coincidentCandidatesByParent[candidateStructureLabel(semNode)];
            agg.first += 1;
            agg.second += cand->second;
        }
        rn.meshBindings = mcIt->second;
        result.scene.nodes[rnId] = std::move(rn);
        return true;
    }

    // Per-material accumulation buffers.
    struct MatGroup {
        std::vector<Vertex> verts;
        std::vector<uint32_t> indices;
    };
    std::map<RenderMaterialId, MatGroup> groups;

    for (const auto &mergeDesc : mergeDescendants) {
        const auto descId = mergeDesc.nodeId;
        const SemanticNode &descNode = scene->nodes.at(descId);
        if (!scene->logVols.contains(descNode.logVolId)) {
            continue;
        }
        const SemanticLogicalVolume &descLv = scene->logVols.at(descNode.logVolId);
        if (!scene->shapes.contains(descLv.shapeId)) {
            continue;
        }

        // Tessellate (using cache) — use per-descendant segment count.
        TessellationParams descParams;
        descParams.maxSegmentsCircle = mergeDesc.maxSegmentsCircle;

        const SemanticShape &descShape = scene->shapes.at(descLv.shapeId);
        const bool descIsBoolean = isBooleanShape(descShape.data);
        auto &descSegMap = meshCache[descLv.shapeId];
        MeshAssetId descMid;
        bool isBBoxProxy = false;
        if (!descSegMap.contains(descParams.maxSegmentsCircle)) {
            TessellationOutput tessOut;
            if (descIsBoolean) {
                tessOut = tessellateBooleanShape(descShape.data, *scene, tess, descParams);
            } else {
                tessOut = tess.tessellate(descShape.data, descParams);
            }
            result.diags.append(tessOut.diags);
            if (descIsBoolean && !tessOut.succeeded) {
                // Boolean tessellation genuinely failed (operands unbuildable
                // or the boolean op errored) — apply fallback. An empty but
                // *succeeded* result (e.g. a wedge cut that removed the whole
                // descendant) is not a failure: it falls through to the
                // empty-vertices skip below and contributes nothing.
                switch (rule.fallback) {
                case BooleanFallback::Fail:
                    result.diags.error(
                        codes::kErrTessBooleanFail,
                        std::format("boolean shape on node '{}' cannot be tessellated "
                                    "(fallback=fail)",
                                    descNode.name),
                        descNode.name);
                    done = true;
                    return false;
                case BooleanFallback::BBox:
                    tessOut = makeBBoxProxy(descShape.data, *scene, tess, descParams);
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
            descSegMap[descParams.maxSegmentsCircle] = descMid;
        } else {
            descMid = descSegMap.at(descParams.maxSegmentsCircle);
        }

        if (!result.scene.meshAssets.contains(descMid)) {
            continue;
        }
        const MeshAsset &srcMesh = result.scene.meshAssets.at(descMid);
        if (srcMesh.vertices.empty()) {
            continue;
        }

        // Resolve material — use red proxy for bbox fallbacks.
        RenderMaterialId rmId = isBBoxProxy ? getBBoxProxyMaterial()
                                            : resolveRenderMaterial(descLv.materialId, descNode);

        // Transform vertices into merge node's local frame
        const glm::mat4 toLocal = glm::mat4(mergeDesc.toMergeLocal);
        const glm::mat3 normalMat = glm::mat3(glm::transpose(glm::inverse(glm::mat3(toLocal))));

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

    if (groups.empty() && !wedgeCutApplied) {
        // With a wedge cut applied, an empty merge is expected: the cut can
        // empty (and the prune can remove) every descendant of a stave whose
        // envelope straddled the cut. Rendering nothing is correct, so only
        // warn when no cut is in play — i.e. the genuine "selection removed
        // them" config case this diagnostic was meant to catch.
        result.diags.warn(
            codes::kWarnTessMergeEmpty,
            std::format("merge_descendants on '{}' produced no geometry -- node has no "
                        "tessellatable descendants (did selection remove them?)",
                        semNode.name),
            semNode.name);
    }

    // drop_coincident_faces: strip exact-coincident, opposite-wound interior faces
    // (the never-visible seams between stacked opaque slabs) BEFORE emitting
    // MeshAssets, so the removed triangles' now-dangling vertices simply never
    // get referenced by the final index buffers. Must run across all groups
    // jointly — see removeCoincidentInteriorFaces()'s comment: the calo's
    // absorber/scintillator interfaces are between *different* materials, so
    // a per-group pass would never see them.
    if (rule.dropCoincidentFaces) {
        const size_t removed = removeCoincidentInteriorFaces(groups);
        if (removed > 0) {
            // Accumulate only — the per-node counts are summed into a single
            // aggregate diagnostic in take(). Emitting one info diag per node
            // here floods the viewer toast area (100+ calo staves).
            coincidentFacesRemoved.fetch_add(removed, std::memory_order_relaxed);
            coincidentNodesAffected.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // drop_coincident_faces is off for this node. Run detection in count-only
        // mode (no mutation) so we can suggest the option when the node is a
        // clear candidate — otherwise the optimisation is easy to miss. Group
        // by parent structure so take() can name which structures are affected.
        const size_t removable = removeCoincidentInteriorFaces(groups, /*apply=*/false);
        if (removable >= kCoincidentCandidateMinFaces) {
            // Count this first (cache-miss) instance, and remember the prototype
            // so later cache hits reusing it credit their own structures too
            // (mergeKey is still alive here — it is moved into mergeCache below).
            auto &agg = coincidentCandidatesByParent[candidateStructureLabel(semNode)];
            agg.first += 1;
            agg.second += removable;
            dropCandidateByKey[mergeKey] = removable;
        }
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

    mergeCache[std::move(mergeKey)] = rn.meshBindings;

    // Don't enqueue children — they've been consumed by the merge.
    result.scene.nodes[rnId] = std::move(rn);
    return true;
}

bool TessellationJob::Impl::tessellateBooleanNode(const SemanticNode &semNode,
                                                  const SemanticLogicalVolume &lv,
                                                  const SemanticShape &shape, RenderNode &rn,
                                                  RenderNodeId rnId,
                                                  const ResolvedTessellation &rule,
                                                  const TessellationParams &params) {
    // Check the mesh cache first — same shapeId + segments → same mesh.
    // The sentinel id 0 (never allocated; ids start at 1) marks a boolean
    // that succeeded but removed the solid entirely (e.g. a placement fully
    // inside an angle-cut wedge): a valid empty result, cached so instances
    // sharing the shape are not re-evaluated.
    constexpr MeshAssetId kFullyRemoved{0};
    auto &boolSegMap = meshCache[lv.shapeId];
    MeshAssetId boolMid;
    bool boolCacheHit = boolSegMap.contains(params.maxSegmentsCircle);
    bool fullyRemoved = false;
    if (boolCacheHit) {
        boolMid = boolSegMap.at(params.maxSegmentsCircle);
        fullyRemoved = (boolMid == kFullyRemoved);
    } else {
        auto boolOut = tessellateBooleanShape(shape.data, *scene, tess, params);
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
        } else if (boolOut.succeeded) {
            // Empty but valid: the solid was fully cut away. Render nothing.
            boolSegMap[params.maxSegmentsCircle] = kFullyRemoved;
            fullyRemoved = true;
        }
    }
    if (fullyRemoved) {
        // No mesh binding — correct, not a failure, so no warning.
        result.scene.nodes[rnId] = std::move(rn);
        for (const auto childId : semNode.children) {
            q.push(childId);
        }
        return true;
    }
    if (boolCacheHit) {
        RenderMaterialId rmId = resolveRenderMaterial(lv.materialId, semNode);
        rn.meshBindings.push_back({boolMid, rmId});
        result.scene.nodes[rnId] = std::move(rn);
        for (const auto childId : semNode.children) {
            q.push(childId);
        }
        return true;
    }
    // Manifold failed or produced empty output — fall through to fallback.
    switch (rule.fallback) {
    case BooleanFallback::Fail:
        result.diags.error(
            codes::kErrTessBooleanFail,
            std::format("boolean shape on node '{}' cannot be tessellated (fallback=fail)",
                        semNode.name),
            semNode.name);
        done = true;
        return false;
    case BooleanFallback::BBox: {
        result.diags.warn(codes::kWarnTessBooleanBbox,
                          std::format("boolean shape on node '{}' replaced with bounding-box proxy",
                                      semNode.name),
                          semNode.name);
        auto bboxOut = makeBBoxProxy(shape.data, *scene, tess, params);
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
    return true;
}

bool TessellationJob::Impl::tessellatePrimitiveNode(const SemanticNode &semNode,
                                                    const SemanticLogicalVolume &lv,
                                                    const SemanticShape &shape, RenderNode &rn,
                                                    RenderNodeId rnId,
                                                    const TessellationParams &params) {
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
    return true;
}

// ── TessellationJob public API ───────────────────────────────────────────────

TessellationJob::TessellationJob() : impl_(std::make_unique<Impl>()) {}
TessellationJob::~TessellationJob() = default;
TessellationJob::TessellationJob(TessellationJob &&) noexcept = default;
TessellationJob &TessellationJob::operator=(TessellationJob &&) noexcept = default;

void TessellationJob::start(const NHConfig &config, const SemanticScene &scene) {
    // std::atomic members make Impl non-copyable; replace the unique_ptr
    // wholesale to reset the job between runs.
    impl_ = std::make_unique<Impl>();
    impl_->config = &config;
    impl_->scene = &scene;
    impl_->wedgeCutApplied =
        std::any_of(scene.logVols.begin(), scene.logVols.end(),
                    [](const auto &kv) { return kv.second.name == kWedgeEmptyLogVolName; });
    // Compile rule match predicates once up front, reused across every resolver
    // call. The resolve* helpers each iterate every rule for every node, so
    // compiling per-call dominated the tessellation profile.
    impl_->compiledRules = compileRulePredicates(config.rules);
    // countEffectiveNodes uses compiledRules + config + scene set above,
    // so it must run after those are wired in.
    impl_->totalNodes.store(impl_->countEffectiveNodes(), std::memory_order_relaxed);
    if (!scene.nodes.empty() && scene.nodes.contains(scene.rootId)) {
        impl_->q.push(scene.rootId);
    } else {
        impl_->done = true;
    }
    impl_->started = true;
}

bool TessellationJob::advance(uint64_t budget_ns) {
    if (!impl_->started) {
        impl_->done = true;
        return true;
    }
    if (impl_->done) {
        return true;
    }
    // Sample the wall clock only once every kClockCheckStride processed nodes.
    // Per-node sampling is wasteful, and — critically — in a Web Worker without
    // cross-origin isolation the monotonic clock can be coarsened to a
    // granularity at or above our slice budget. A per-node check would then let
    // a single node's elapsed read exceed the budget and collapse the slice to
    // one node per advance() call; a caller that emits progress per slice (the
    // compute worker) would in turn flood its postMessage channel and appear
    // frozen. Batching the check guarantees at least kClockCheckStride nodes of
    // forward progress per call regardless of clock resolution.
    constexpr unsigned kClockCheckStride = 128;
    const auto start_time = std::chrono::steady_clock::now();
    unsigned since_check = 0;
    while (!impl_->q.empty() && !impl_->done) {
        if (!impl_->stepOneNode()) {
            // stepOneNode returned false: queue popped a stale id (no-op
            // iteration) or the pass aborted with done=true. Either way,
            // fall through to the loop condition.
            continue;
        }
        if (++since_check < kClockCheckStride) {
            continue;
        }
        since_check = 0;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - start_time)
                                 .count();
        if (static_cast<uint64_t>(elapsed) >= budget_ns) {
            break;
        }
    }
    if (impl_->q.empty()) {
        impl_->done = true;
    }
    return impl_->done;
}

TessellationPassResult TessellationJob::take() {
    std::println(stderr, "Tessellation stats:");
    std::println(stderr, "  Render nodes:     {}", impl_->result.scene.nodes.size());
    std::println(stderr, "  Unique meshes:    {}", impl_->result.scene.meshAssets.size());
    std::println(stderr, "  Unique materials: {}", impl_->result.scene.materials.size());
    std::println(stderr, "  Mesh cache entries (shapes with tessellation): {}",
                 impl_->meshCache.size());
    const size_t coincRemoved = impl_->coincidentFacesRemoved.load(std::memory_order_relaxed);
    std::println(stderr, "  Coincident interior faces removed (drop_coincident_faces): {}",
                 coincRemoved);
    // Single aggregate diagnostic for the whole pass (one toast), replacing the
    // former per-node info diags that flooded the viewer.
    if (coincRemoved > 0) {
        const size_t coincNodes = impl_->coincidentNodesAffected.load(std::memory_order_relaxed);
        impl_->result.diags.info(
            codes::kInfoTessCoincidentRemoved,
            std::format("drop_coincident_faces removed {} interior face(s) ({} coincident, "
                        "opposite-wound triangle pair(s)) across {} node(s)",
                        coincRemoved, coincRemoved / 2, coincNodes),
            "drop_coincident_faces");
    }
    // Discoverability suggestion: merge_descendants nodes that would shed a
    // meaningful number of interior faces but don't have drop_coincident_faces on.
    // One aggregate diagnostic for the whole pass, grouped by parent structure
    // so it names *what* is affected without ever flooding.
    if (!impl_->coincidentCandidatesByParent.empty()) {
        std::vector<std::pair<std::string, std::pair<size_t, size_t>>> groups(
            impl_->coincidentCandidatesByParent.begin(), impl_->coincidentCandidatesByParent.end());
        // Most-impactful structures first.
        std::sort(groups.begin(), groups.end(),
                  [](const auto &a, const auto &b) { return a.second.second > b.second.second; });

        size_t totalNodes = 0;
        size_t totalFaces = 0;
        for (const auto &[label, agg] : groups) {
            totalNodes += agg.first;
            totalFaces += agg.second;
        }

        // Name the biggest structures; cap the list so a pathological scene with
        // hundreds of distinct candidate structures can't blow up the message.
        constexpr size_t kMaxStructuresListed = 6;
        std::string list;
        for (size_t i = 0; i < groups.size() && i < kMaxStructuresListed; ++i) {
            if (!list.empty()) {
                list += "; ";
            }
            list += std::format("{} ({} node(s), {} face(s))", groups[i].first,
                                groups[i].second.first, groups[i].second.second);
        }
        if (groups.size() > kMaxStructuresListed) {
            list += std::format("; +{} more structure(s)", groups.size() - kMaxStructuresListed);
        }

        std::println(
            stderr,
            "  drop_coincident_faces candidate: {} removable interior face(s) across {} node(s) "
            "in {} structure(s)",
            totalFaces, totalNodes, groups.size());
        impl_->result.diags.warn(
            codes::kWarnTessCoincidentCandidate,
            std::format(
                "drop_coincident_faces is off but would remove {} interior face(s) from {} "
                "merge_descendants node(s) -- candidates: {}. Enable "
                "'drop_coincident_faces = true' on these structures to cut interior overdraw",
                totalFaces, totalNodes, list),
            "drop_coincident_faces");
    }
    return std::move(impl_->result);
}

size_t TessellationJob::totalNodes() const {
    return impl_->totalNodes.load(std::memory_order_relaxed);
}
size_t TessellationJob::processedNodes() const {
    return impl_->processedNodes.load(std::memory_order_relaxed);
}

// ── TessellationPass::lower (run-to-completion shim) ─────────────────────────

TessellationPassResult TessellationPass::lower(const SemanticScene &scene) const {
    TessellationJob job;
    job.start(config_, scene);
    while (!job.advance(std::numeric_limits<uint64_t>::max())) {
        // job.advance never returns false unless the queue still has work
        // *and* we hit a budget; with budget=max we'll always complete in
        // a single call, but loop defensively.
    }
    return job.take();
}

} // namespace nodehammer
