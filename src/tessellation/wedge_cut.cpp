#include <nodehammer/tessellation/wedge_cut.hpp>

#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <ankerl/unordered_dense.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <numbers>
#include <variant>
#include <vector>

namespace nodehammer {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr double kTau = 2.0 * kPi;
constexpr int kMaxBoolDepth = 32;

// ── Angle helpers (mirrors the viewer's CPU angle-cut classification) ─────────

double normAngle(double a) {
    a = std::fmod(a, kTau);
    if (a < 0.0) {
        a += kTau;
    }
    return a;
}

/// CCW span from `start` to `end`, always in (0, tau].
double sectorSpan(double start, double end) {
    const double s = normAngle(end) - normAngle(start);
    return s > 0.0 ? s : s + kTau;
}

bool angleInRange(double angle, double start, double end) {
    angle = normAngle(angle);
    start = normAngle(start);
    end = normAngle(end);
    if (std::abs(start - end) < 1e-9) {
        return false;
    }
    if (start <= end) {
        return angle >= start && angle <= end;
    }
    return angle >= start || angle <= end;
}

/// True iff the xy projection of the AABB lies entirely within the angular
/// sector [start, end] (CCW). Conservative: a box spanning the z-axis covers
/// all angles and is never "fully inside" any proper sector.
bool aabbFullyInsideSector(const glm::dvec3 &lo, const glm::dvec3 &hi, double start, double end) {
    if (std::abs(normAngle(start) - normAngle(end)) < 1e-9) {
        return false;
    }
    if (lo.x <= 0.0 && hi.x >= 0.0 && lo.y <= 0.0 && hi.y >= 0.0) {
        return false; // contains the z-axis → spans every angle
    }

    // For sectors wider than a half-turn, testing the (narrow) complementary
    // sector is numerically cleaner: "inside the wide sector" == "no corner in
    // the narrow complement".
    const double width = sectorSpan(start, end);
    const bool testKept = width > 0.5 * kTau;
    const double tStart = testKept ? end : start;
    const double tEnd = testKept ? start : end;

    const std::array<glm::dvec2, 4> corners{{
        {lo.x, lo.y},
        {lo.x, hi.y},
        {hi.x, lo.y},
        {hi.x, hi.y},
    }};
    for (const auto &c : corners) {
        const bool inTest = angleInRange(std::atan2(c.y, c.x), tStart, tEnd);
        if (testKept) {
            if (inTest) {
                return false;
            }
        } else if (!inTest) {
            return false;
        }
    }
    return true;
}

// ── Conservative local-frame AABB per shape ───────────────────────────────────

struct Aabb {
    glm::dvec3 lo{0.0};
    glm::dvec3 hi{0.0};
    bool valid{false};
};

Aabb sym(double x, double y, double z) { return Aabb{{-x, -y, -z}, {x, y, z}, true}; }

Aabb mergeAabb(const Aabb &a, const Aabb &b) {
    if (!a.valid) {
        return b;
    }
    if (!b.valid) {
        return a;
    }
    return Aabb{glm::min(a.lo, b.lo), glm::max(a.hi, b.hi), true};
}

Aabb transformAabb(const Aabb &a, const glm::dmat4 &m) {
    if (!a.valid) {
        return a;
    }
    Aabb out;
    for (int i = 0; i < 8; ++i) {
        const glm::dvec3 corner{
            (i & 1) ? a.hi.x : a.lo.x,
            (i & 2) ? a.hi.y : a.lo.y,
            (i & 4) ? a.hi.z : a.lo.z,
        };
        const glm::dvec3 p = glm::dvec3{m * glm::dvec4{corner, 1.0}};
        if (!out.valid) {
            out.lo = out.hi = p;
            out.valid = true;
        } else {
            out.lo = glm::min(out.lo, p);
            out.hi = glm::max(out.hi, p);
        }
    }
    return out;
}

Aabb localAabb(const SemanticShapeVariant &shape, const SemanticScene &scene, int depth);

Aabb localAabbById(SemanticShapeId id, const SemanticScene &scene, int depth) {
    auto it = scene.shapes.find(id);
    if (it == scene.shapes.end()) {
        return Aabb{};
    }
    return localAabb(it->second.data, scene, depth);
}

Aabb localAabb(const SemanticShapeVariant &shape, const SemanticScene &scene, int depth) {
    if (depth > kMaxBoolDepth) {
        return Aabb{};
    }
    using detail::overloaded;
    return std::visit(
        overloaded{
            [](const BoxShape &s) { return sym(s.dx, s.dy, s.dz); },
            [](const TubeShape &s) { return sym(s.rMax, s.rMax, s.dz); },
            [](const ConeShape &s) {
                const double r = std::max(s.rMax1, s.rMax2);
                return sym(r, r, s.dz);
            },
            [](const TrdShape &s) {
                return sym(std::max(s.dx1, s.dx2), std::max(s.dy1, s.dy2), s.dz);
            },
            [](const ParaShape &s) {
                const double ex = s.dx + std::abs(s.dy * std::tan(s.alpha)) +
                                  std::abs(s.dz * std::tan(s.theta) * std::cos(s.phi));
                const double ey = s.dy + std::abs(s.dz * std::tan(s.theta) * std::sin(s.phi));
                return sym(ex, ey, s.dz);
            },
            [](const PconShape &s) -> Aabb {
                if (s.sections.empty()) {
                    return Aabb{};
                }
                double r = 0.0, zlo = s.sections.front().z, zhi = s.sections.front().z;
                for (const auto &sec : s.sections) {
                    r = std::max(r, sec.rMax);
                    zlo = std::min(zlo, sec.z);
                    zhi = std::max(zhi, sec.z);
                }
                return Aabb{{-r, -r, zlo}, {r, r, zhi}, true};
            },
            [](const PgonShape &s) -> Aabb {
                if (s.sections.empty()) {
                    return Aabb{};
                }
                double r = 0.0, zlo = s.sections.front().z, zhi = s.sections.front().z;
                for (const auto &sec : s.sections) {
                    r = std::max(r, sec.rMax);
                    zlo = std::min(zlo, sec.z);
                    zhi = std::max(zhi, sec.z);
                }
                // rMax is the apothem (flat radius); circumscribe to be safe.
                const int n = std::max(s.nSides, 3);
                r /= std::cos(kPi / static_cast<double>(n));
                return Aabb{{-r, -r, zlo}, {r, r, zhi}, true};
            },
            [](const TorusShape &s) {
                const double outer = s.rTor + s.rMax;
                return Aabb{{-outer, -outer, -s.rMax}, {outer, outer, s.rMax}, true};
            },
            [](const TessellatedShape &s) -> Aabb {
                Aabb out;
                for (const auto &tri : s.triangles) {
                    for (const auto &v : tri.vertices) {
                        if (!out.valid) {
                            out.lo = out.hi = v;
                            out.valid = true;
                        } else {
                            out.lo = glm::min(out.lo, v);
                            out.hi = glm::max(out.hi, v);
                        }
                    }
                }
                return out;
            },
            [&](const BooleanUnion &s) {
                const Aabb l = localAabbById(s.left, scene, depth + 1);
                const Aabb r =
                    transformAabb(localAabbById(s.right, scene, depth + 1), s.rightTransform);
                return mergeAabb(l, r);
            },
            // result ⊆ left for both subtraction and intersection
            [&](const BooleanIntersection &s) { return localAabbById(s.left, scene, depth + 1); },
            [&](const BooleanSubtraction &s) { return localAabbById(s.left, scene, depth + 1); },
            [](const UnknownShape &) { return Aabb{}; },
        },
        shape);
}

// ── Cut deduplication key ─────────────────────────────────────────────────────
//
// Two straddling placements need the *same* cut iff they reference the same
// original shape and the wedge, transformed into their local frame, is identical.
// Because the wedge is a z-prism about the global axis with bounds chosen to
// clear all geometry, the only thing that varies is the pair of bounding
// half-planes (each through the global z-axis). We key on the original shape id
// plus a quantized signature of those two planes in the placement's local frame.
// Instances related by a translation along the cut axis (barrel z-ladders) yield
// identical signatures and therefore share one cut mesh.

/// Quantize a world-space plane transformed into a placement's local frame.
/// Planes transform by the (math) transpose of the local→world matrix.
std::array<int64_t, 4> planeSignature(const glm::dvec4 &planeWorld, const glm::dmat4 &world) {
    glm::dvec4 pl = glm::transpose(world) * planeWorld;
    const double len = glm::length(glm::dvec3{pl});
    if (len > 0.0) {
        pl /= len;
    }
    return {std::llround(pl.x * 1e6), std::llround(pl.y * 1e6), std::llround(pl.z * 1e6),
            std::llround(pl.w * 1e3)};
}

struct CutKey {
    uint64_t shapeId{0};
    std::array<int64_t, 4> planeA{};
    std::array<int64_t, 4> planeB{};
    bool operator==(const CutKey &) const = default;
};

struct CutKeyHash {
    std::size_t operator()(const CutKey &k) const noexcept {
        std::size_t h = std::hash<uint64_t>{}(k.shapeId);
        auto mix = [&h](int64_t v) {
            h ^= std::hash<uint64_t>{}(static_cast<uint64_t>(v)) + 0x9e3779b97f4a7c15ULL +
                 (h << 6) + (h >> 2);
        };
        for (int64_t v : k.planeA) {
            mix(v);
        }
        for (int64_t v : k.planeB) {
            mix(v);
        }
        return h;
    }
};

} // namespace

WedgeCutStats applyWedgeCut(SemanticScene &scene, const WedgeCutParams &params,
                            DiagnosticList &diags) {
    WedgeCutStats stats;

    const double startRad = params.startDeg * kPi / 180.0;
    const double endRad = params.endDeg * kPi / 180.0;
    const double removedWidth = sectorSpan(startRad, endRad);

    // Degenerate sector: nothing (or everything) removed → leave scene untouched.
    if (removedWidth < 1e-6 || removedWidth > kTau - 1e-6) {
        return stats;
    }

    scene.computeWorldTransforms();
    // Loaded scenes carry stale ID counters; ensure our new shapes/logVols get
    // fresh IDs rather than overwriting existing entries.
    scene.reseedIdCounters();

    // ── Pass 1: global bounds for sizing the cutting solid ────────────────────
    double maxR = 0.0;
    double maxZ = 0.0;
    bool anyBounds = false;
    for (const auto &[id, node] : scene.nodes) {
        const auto lvIt = scene.logVols.find(node.logVolId);
        if (lvIt == scene.logVols.end()) {
            continue;
        }
        const Aabb world =
            transformAabb(localAabbById(lvIt->second.shapeId, scene, 0), node.worldTransform);
        if (!world.valid) {
            continue;
        }
        for (int i = 0; i < 4; ++i) {
            const double x = (i & 1) ? world.hi.x : world.lo.x;
            const double y = (i & 2) ? world.hi.y : world.lo.y;
            maxR = std::max(maxR, std::hypot(x, y));
        }
        maxZ = std::max({maxZ, std::abs(world.lo.z), std::abs(world.hi.z)});
        anyBounds = true;
    }
    if (!anyBounds) {
        return stats;
    }

    const double margin = std::max(params.margin, 1.0);
    const double cutR = std::max(margin * maxR, 1.0);
    const double cutZ = std::max(margin * maxZ, 1.0);

    // ── Shared cutting solid: a phi-sector tube covering the removed wedge ─────
    const SemanticShapeId wedgeId = scene.nextShapeId();
    scene.shapes[wedgeId] =
        SemanticShape{wedgeId, TubeShape{0.0, cutR, cutZ, startRad, removedWidth}};

    // ── Shared empty shape for placements fully inside the removed sector ─────
    const SemanticShapeId emptyShapeId = scene.nextShapeId();
    scene.shapes[emptyShapeId] = SemanticShape{emptyShapeId, TessellatedShape{}};
    const SemanticMaterialId fillerMat =
        scene.materials.empty() ? SemanticMaterialId{} : scene.materials.begin()->first;
    const SemanticLogVolId emptyLvId = scene.nextLogVolId();
    scene.logVols[emptyLvId] =
        SemanticLogicalVolume{emptyLvId, "__wedge_empty", emptyShapeId, fillerMat};

    // The two bounding half-planes of the removed sector, through the global
    // z-axis (normal in xy, d = 0). Used to build per-placement cut signatures.
    const glm::dvec4 planeStart{-std::sin(startRad), std::cos(startRad), 0.0, 0.0};
    const glm::dvec4 planeEnd{-std::sin(endRad), std::cos(endRad), 0.0, 0.0};

    // Cut-shape dedup cache: identical (shape, local-frame wedge) → shared cut
    // logVol, so instances that need the same cut stay instanced.
    ankerl::unordered_dense::map<CutKey, SemanticLogVolId, CutKeyHash> cutCache;

    // Per-node "produces its own geometry" flag (kept or cut), used afterwards to
    // prune subtrees the cut removes entirely. Emptied/skipped nodes produce no
    // mesh; nodes we cannot classify default to "has geometry" (never pruned).
    ankerl::unordered_dense::map<SemanticNodeId, bool> hasOwnGeom;

    // ── Pass 2: classify each placement and rewrite its logVol binding ────────
    // We only mutate scene.nodes values (logVolId) in place and *insert* into
    // scene.shapes / scene.logVols — iterating scene.nodes stays valid. Fields
    // read from the (reference-returning) maps are copied out before any insert
    // that could rehash them.
    for (auto &[id, node] : scene.nodes) {
        const auto lvIt = scene.logVols.find(node.logVolId);
        if (lvIt == scene.logVols.end()) {
            continue;
        }
        const SemanticShapeId origShapeId = lvIt->second.shapeId;
        const SemanticMaterialId origMat = lvIt->second.materialId;
        const std::string lvName = lvIt->second.name;

        const Aabb local = localAabbById(origShapeId, scene, 0);
        if (!local.valid) {
            ++stats.skipped;
            hasOwnGeom[id] = false; // unbounded shapes produce no tessellatable mesh
            continue;
        }
        const Aabb world = transformAabb(local, node.worldTransform);

        if (aabbFullyInsideSector(world.lo, world.hi, startRad, endRad)) {
            // Fully inside the removed sector → render nothing.
            node.logVolId = emptyLvId;
            ++stats.emptied;
            hasOwnGeom[id] = false;
        } else if (aabbFullyInsideSector(world.lo, world.hi, endRad, startRad)) {
            // Fully inside the kept sector → leave shared (instanced).
            ++stats.kept;
            hasOwnGeom[id] = true;
        } else {
            // Straddles the boundary → boolean-subtraction cut. Reuse an existing
            // cut shape when this placement's (shape, local-frame wedge) matches
            // one already created, so instanced copies needing the same cut stay
            // instanced; otherwise create a fresh cut shape in this placement's
            // local frame.
            const CutKey key{origShapeId.value, planeSignature(planeStart, node.worldTransform),
                             planeSignature(planeEnd, node.worldTransform)};
            ++stats.cut;
            if (auto it = cutCache.find(key); it != cutCache.end()) {
                node.logVolId = it->second;
            } else {
                const SemanticShapeId cutShapeId = scene.nextShapeId();
                scene.shapes[cutShapeId] = SemanticShape{
                    cutShapeId,
                    BooleanSubtraction{origShapeId, wedgeId, glm::inverse(node.worldTransform)}};
                const SemanticLogVolId cutLvId = scene.nextLogVolId();
                scene.logVols[cutLvId] =
                    SemanticLogicalVolume{cutLvId, lvName + "_wedgecut", cutShapeId, origMat};
                cutCache.emplace(key, cutLvId);
                node.logVolId = cutLvId;
                ++stats.cutUnique;
            }
            hasOwnGeom[id] = true;
        }
    }

    // ── Prune fully-cut-away subtrees ─────────────────────────────────────────
    // A subtree that contains no kept/cut node produces nothing; removing it
    // keeps the render tree clean and, importantly, prevents merge_descendants
    // parents (e.g. a stave entirely inside the removed wedge) from being asked
    // to merge an all-empty set of children (which would emit NH0505).
    if (scene.nodes.contains(scene.rootId)) {
        ankerl::unordered_dense::map<SemanticNodeId, bool> subtreeGeom;
        auto computeGeom = [&](auto &&self, SemanticNodeId nid) -> bool {
            const auto it = scene.nodes.find(nid);
            if (it == scene.nodes.end()) {
                return false;
            }
            // Default to "has geometry" for nodes we never classified (e.g. logVol
            // missing) so we never prune something we don't understand.
            bool g = hasOwnGeom.contains(nid) ? hasOwnGeom.at(nid) : true;
            for (const auto childId : it->second.children) {
                g = self(self, childId) || g;
            }
            subtreeGeom[nid] = g;
            return g;
        };
        computeGeom(computeGeom, scene.rootId);

        // Collect maximal empty subtree roots: an empty node whose parent keeps
        // geometry. (The root itself is never removed.)
        std::vector<SemanticNodeId> removalRoots;
        for (const auto &[id, geom] : subtreeGeom) {
            if (geom || id == scene.rootId) {
                continue;
            }
            const auto nit = scene.nodes.find(id);
            if (nit == scene.nodes.end() || !nit->second.parentId) {
                continue;
            }
            const auto pit = subtreeGeom.find(*nit->second.parentId);
            if (pit != subtreeGeom.end() && pit->second) {
                removalRoots.push_back(id);
            }
        }

        for (const auto rootOfEmpty : removalRoots) {
            const auto nit = scene.nodes.find(rootOfEmpty);
            if (nit == scene.nodes.end()) {
                continue;
            }
            // Detach from parent.
            if (nit->second.parentId) {
                auto pit = scene.nodes.find(*nit->second.parentId);
                if (pit != scene.nodes.end()) {
                    auto &kids = pit->second.children;
                    kids.erase(std::remove(kids.begin(), kids.end(), rootOfEmpty), kids.end());
                }
            }
            // Erase the whole subtree.
            std::vector<SemanticNodeId> stack{rootOfEmpty};
            while (!stack.empty()) {
                const auto cur = stack.back();
                stack.pop_back();
                const auto cit = scene.nodes.find(cur);
                if (cit == scene.nodes.end()) {
                    continue;
                }
                for (const auto childId : cit->second.children) {
                    stack.push_back(childId);
                }
                scene.nodes.erase(cit);
                ++stats.pruned;
            }
        }
    }

    diags.info(codes::kInfoWedgeCutApplied,
               std::format("wedge cut [{:.1f}°,{:.1f}°]: {} cut ({} unique), {} emptied, {} kept, "
                           "{} skipped, {} pruned",
                           params.startDeg, params.endDeg, stats.cut, stats.cutUnique,
                           stats.emptied, stats.kept, stats.skipped, stats.pruned));
    return stats;
}

} // namespace nodehammer
