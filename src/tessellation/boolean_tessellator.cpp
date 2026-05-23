#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/tessellation/boolean_tessellator.hpp>

#include <manifold/manifold.h>

#include <nodehammer/detail/overloaded.hpp>

#include <cstring>
#include <format>
#include <map>
#include <numbers>
#include <optional>

namespace nodehammer {

namespace {

constexpr int kMaxRecursionDepth = 32;

} // namespace

// ── Mesh conversion helpers (public) ─────────────────────────────────────────

std::optional<manifold::Manifold> meshToManifold(const TessellationOutput &mesh,
                                                 DiagnosticList &diags, std::string_view context) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        return std::nullopt;
    }

    manifold::MeshGL mgl;
    mgl.numProp = 3;
    mgl.vertProperties.resize(mesh.vertices.size() * 3);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        mgl.vertProperties[i * 3 + 0] = mesh.vertices[i].position.x;
        mgl.vertProperties[i * 3 + 1] = mesh.vertices[i].position.y;
        mgl.vertProperties[i * 3 + 2] = mesh.vertices[i].position.z;
    }
    mgl.triVerts.resize(mesh.indices.size());
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        mgl.triVerts[i] = mesh.indices[i];
    }

    // Build merge vectors: find vertices at the same position and tell Manifold
    // they should be considered topologically identical.
    // Use a map from quantized position to the first vertex index seen at that position.
    struct PosKey {
        int64_t x, y, z;
        bool operator<(const PosKey &o) const {
            return std::tie(x, y, z) < std::tie(o.x, o.y, o.z);
        }
    };
    // Quantize to ~1e-7 resolution (sufficient for geometry in mm/cm range).
    // Use a double accumulator and 64-bit keys: float (24-bit mantissa) cannot
    // represent the scaled value, and a 32-bit key overflows, for coordinates
    // beyond ~200 mm — both silently corrupt the merge for large geometry such
    // as the calorimeter envelopes.
    constexpr double kScale = 1e7;
    std::map<PosKey, uint32_t> posMap;

    for (uint32_t i = 0; i < static_cast<uint32_t>(mesh.vertices.size()); ++i) {
        const auto &p = mesh.vertices[i].position;
        PosKey key{static_cast<int64_t>(std::llround(static_cast<double>(p.x) * kScale)),
                   static_cast<int64_t>(std::llround(static_cast<double>(p.y) * kScale)),
                   static_cast<int64_t>(std::llround(static_cast<double>(p.z) * kScale))};
        auto [it, inserted] = posMap.emplace(key, i);
        if (!inserted && it->second != i) {
            // This vertex duplicates an earlier one — record the merge pair.
            mgl.mergeFromVert.push_back(i);
            mgl.mergeToVert.push_back(it->second);
        }
    }

    try {
        auto m = manifold::Manifold(mgl);
        if (m.Status() != manifold::Manifold::Error::NoError) {
            diags.warn(codes::kWarnTessBooleanManifoldFail,
                       std::format("non-manifold input mesh for '{}' (status {})", context,
                                   static_cast<int>(m.Status())),
                       std::string{context});
            return std::nullopt;
        }
        return m;
    } catch (const std::exception &e) {
        diags.warn(codes::kWarnTessBooleanManifoldFail,
                   std::format("manifold construction failed for '{}': {}", context, e.what()),
                   std::string{context});
        return std::nullopt;
    }
}

namespace {

/// Convert a Manifold result back to a TessellationOutput.
/// Recomputes face normals (flat shading) since the boolean op invalidates input normals.
TessellationOutput manifoldToMesh(const manifold::Manifold &m) {
    TessellationOutput out;
    auto mgl = m.GetMeshGL();

    if (mgl.triVerts.empty()) {
        return out;
    }

    // Each triangle gets 3 unique vertices with the face normal (flat shading).
    const std::size_t numTris = mgl.triVerts.size() / 3;
    out.vertices.reserve(numTris * 3);
    out.indices.reserve(numTris * 3);

    for (std::size_t t = 0; t < numTris; ++t) {
        const auto i0 = mgl.triVerts[t * 3 + 0];
        const auto i1 = mgl.triVerts[t * 3 + 1];
        const auto i2 = mgl.triVerts[t * 3 + 2];

        const glm::vec3 p0{mgl.vertProperties[i0 * mgl.numProp + 0],
                           mgl.vertProperties[i0 * mgl.numProp + 1],
                           mgl.vertProperties[i0 * mgl.numProp + 2]};
        const glm::vec3 p1{mgl.vertProperties[i1 * mgl.numProp + 0],
                           mgl.vertProperties[i1 * mgl.numProp + 1],
                           mgl.vertProperties[i1 * mgl.numProp + 2]};
        const glm::vec3 p2{mgl.vertProperties[i2 * mgl.numProp + 0],
                           mgl.vertProperties[i2 * mgl.numProp + 1],
                           mgl.vertProperties[i2 * mgl.numProp + 2]};

        const glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));

        const auto base = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back({p0, normal});
        out.vertices.push_back({p1, normal});
        out.vertices.push_back({p2, normal});
        out.indices.push_back(base);
        out.indices.push_back(base + 1);
        out.indices.push_back(base + 2);
    }

    return out;
}

/// Convert glm::dmat4 to manifold::mat3x4 (la::mat<double,3,4>).
/// Both GLM and la::mat are column-major. mat3x4 has 4 columns of vec3.
manifold::mat3x4 toManifoldTransform(const glm::dmat4 &m) {
    // GLM m[col][row], la::mat {col0, col1, col2, col3} each vec<double,3>
    return manifold::mat3x4({m[0][0], m[0][1], m[0][2]}, // col 0
                            {m[1][0], m[1][1], m[1][2]}, // col 1
                            {m[2][0], m[2][1], m[2][2]}, // col 2
                            {m[3][0], m[3][1], m[3][2]}  // col 3 (translation)
    );
}

// ── Shape → Manifold conversion ─────────────────────────────────────────────

/// Try to create a Manifold from a primitive shape using Manifold's built-in
/// constructors. Returns nullopt for shapes Manifold doesn't natively support.
std::optional<manifold::Manifold> shapeToManifoldBuiltin(const SemanticShapeVariant &shape,
                                                         const TessellationParams &params) {
    using detail::overloaded;
    using MaybeManifold = std::optional<manifold::Manifold>;
    const int segments = params.maxSegmentsCircle;

    return std::visit(
        overloaded{
            // Box → Cube
            [](const BoxShape &box) -> MaybeManifold {
                return manifold::Manifold::Cube({box.dx * 2, box.dy * 2, box.dz * 2}, true);
            },

            // Tube → Cylinder (with optional hollow / partial-phi clipping)
            [&](const TubeShape &tube) -> MaybeManifold {
                const bool fullPhi = std::abs(tube.phiDelta - 2.0 * std::numbers::pi) < 1e-6 &&
                                     std::abs(tube.phiStart) < 1e-6;
                if (fullPhi && tube.rMin < 1e-9) {
                    return manifold::Manifold::Cylinder(tube.dz * 2, tube.rMax, tube.rMax, segments,
                                                        true);
                }
                // Full phi, rMin > 0 → hollow tube via subtraction
                if (fullPhi && tube.rMin > 1e-9) {
                    auto outer = manifold::Manifold::Cylinder(tube.dz * 2, tube.rMax, tube.rMax,
                                                              segments, true);
                    auto inner = manifold::Manifold::Cylinder(tube.dz * 2, tube.rMin, tube.rMin,
                                                              segments, true);
                    return outer - inner;
                }
                // Partial phi: construct as (full cylinder ∩ wedge), optionally - inner.
                const double r = std::max(tube.rMax, tube.rMin) * 2; // oversized for safe clipping
                const double h = tube.dz * 2;

                auto outer = manifold::Manifold::Cylinder(h, tube.rMax, tube.rMax, segments, true);

                // Build a wedge by intersecting two half-space boxes.
                auto bigBox = manifold::Manifold::Cube({r * 2, r * 2, h * 2}, true);

                // The kept sector is the CCW span [phiStart, phiStart+phiDelta],
                // matching the (cos φ, sin φ) sweep used everywhere else (e.g. the
                // primitive tessellator). Each bounding box is the half-space on
                // the sector's interior side of one boundary ray, through the z
                // axis. Box1 keeps the CCW side of the start ray; box2 keeps the CW
                // side of the end ray. The shift places the cube so the kept half
                // is on the correct side *before* the rotation aligns the boundary
                // plane with the ray (getting these signs wrong rotates the whole
                // sector 180°).
                manifold::Manifold box1;
                {
                    const double c = std::cos(tube.phiStart), s = std::sin(tube.phiStart);
                    auto shift = manifold::mat3x4(manifold::vec3{1, 0, 0}, manifold::vec3{0, 1, 0},
                                                  manifold::vec3{0, 0, 1}, manifold::vec3{0, r, 0});
                    auto rot = manifold::mat3x4(manifold::vec3{c, s, 0}, manifold::vec3{-s, c, 0},
                                                manifold::vec3{0, 0, 1}, manifold::vec3{0, 0, 0});
                    box1 = bigBox.Transform(shift).Transform(rot);
                }

                manifold::Manifold box2;
                {
                    const double endPhi = tube.phiStart + tube.phiDelta;
                    const double c = std::cos(endPhi), s = std::sin(endPhi);
                    auto shift =
                        manifold::mat3x4(manifold::vec3{1, 0, 0}, manifold::vec3{0, 1, 0},
                                         manifold::vec3{0, 0, 1}, manifold::vec3{0, -r, 0});
                    auto rot = manifold::mat3x4(manifold::vec3{c, s, 0}, manifold::vec3{-s, c, 0},
                                                manifold::vec3{0, 0, 1}, manifold::vec3{0, 0, 0});
                    box2 = bigBox.Transform(shift).Transform(rot);
                }

                manifold::Manifold wedge;
                if (tube.phiDelta <= std::numbers::pi) {
                    wedge = box1 ^ box2;
                } else {
                    wedge = box1 + box2;
                }

                auto result = outer ^ wedge;

                if (tube.rMin > 1e-9) {
                    auto inner =
                        manifold::Manifold::Cylinder(h, tube.rMin, tube.rMin, segments, true);
                    result = result - inner;
                }

                return result;
            },

            // Cone (full phi, rMin1=0, rMin2=0) → Cylinder with different radii
            [&](const ConeShape &cone) -> MaybeManifold {
                const bool fullPhi = std::abs(cone.phiDelta - 2.0 * std::numbers::pi) < 1e-6 &&
                                     std::abs(cone.phiStart) < 1e-6;
                if (fullPhi && cone.rMin1 < 1e-9 && cone.rMin2 < 1e-9) {
                    return manifold::Manifold::Cylinder(cone.dz * 2, cone.rMax1, cone.rMax2,
                                                        segments, true);
                }
                return std::nullopt;
            },

            // Trd → hull of 8 vertices (guaranteed watertight)
            [](const TrdShape &trd) -> MaybeManifold {
                std::vector<manifold::vec3> pts = {
                    {-trd.dx1, -trd.dy1, -trd.dz}, {trd.dx1, -trd.dy1, -trd.dz},
                    {trd.dx1, trd.dy1, -trd.dz},   {-trd.dx1, trd.dy1, -trd.dz},
                    {-trd.dx2, -trd.dy2, trd.dz},  {trd.dx2, -trd.dy2, trd.dz},
                    {trd.dx2, trd.dy2, trd.dz},    {-trd.dx2, trd.dy2, trd.dz},
                };
                return manifold::Manifold::Hull(pts);
            },

            // All other shapes: no built-in available
            [](const auto &) -> MaybeManifold { return std::nullopt; },
        },
        shape);
}

// ── Recursive resolution ────────────────────────────────────────────────────

/// Recursively resolve a shape ID to a Manifold.
/// For primitives: tries Manifold built-in, falls back to tessellator.
/// For booleans: recurses into operands.
std::optional<manifold::Manifold> resolveAndTessellate(SemanticShapeId shapeId,
                                                       const SemanticScene &scene,
                                                       const ITessellator &tessellator,
                                                       const TessellationParams &params,
                                                       DiagnosticList &diags, int depth) {
    if (depth > kMaxRecursionDepth) {
        diags.warn(codes::kWarnTessBooleanManifoldFail,
                   std::format("boolean recursion depth exceeded (max {})", kMaxRecursionDepth));
        return std::nullopt;
    }

    if (!scene.shapes.contains(shapeId)) {
        diags.warn(codes::kWarnTessBooleanManifoldFail,
                   std::format("shape ID {} not found in scene", shapeId.value));
        return std::nullopt;
    }

    const auto &shapeData = scene.shapes.at(shapeId).data;

    // Boolean shapes: recurse
    auto handleBoolean = [&](const auto &boolShape) -> std::optional<manifold::Manifold> {
        auto left =
            resolveAndTessellate(boolShape.left, scene, tessellator, params, diags, depth + 1);
        if (!left) {
            return std::nullopt;
        }
        auto right =
            resolveAndTessellate(boolShape.right, scene, tessellator, params, diags, depth + 1);
        if (!right) {
            return std::nullopt;
        }

        // Apply right operand transform
        if (boolShape.rightTransform != glm::dmat4{1.0}) {
            *right = right->Transform(toManifoldTransform(boolShape.rightTransform));
        }

        using T = std::decay_t<decltype(boolShape)>;
        if constexpr (std::is_same_v<T, BooleanUnion>) {
            return *left + *right;
        } else if constexpr (std::is_same_v<T, BooleanSubtraction>) {
            return *left - *right;
        } else if constexpr (std::is_same_v<T, BooleanIntersection>) {
            return *left ^ *right;
        }
    };

    {
        using detail::overloaded;
        auto boolResult = std::visit(
            overloaded{
                [&](const BooleanUnion &s) { return handleBoolean(s); },
                [&](const BooleanSubtraction &s) { return handleBoolean(s); },
                [&](const BooleanIntersection &s) { return handleBoolean(s); },
                [](const auto &) -> std::optional<manifold::Manifold> { return std::nullopt; },
            },
            shapeData);
        if (boolResult) {
            return boolResult;
        }
    }

    // Primitive shape: try Manifold built-in first
    auto builtin = shapeToManifoldBuiltin(shapeData, params);
    if (builtin) {
        return builtin;
    }

    // Fall back to our tessellator → convert mesh to Manifold
    auto tessOut = tessellator.tessellate(shapeData, params);
    diags.append(tessOut.diags);
    return meshToManifold(tessOut, diags, std::format("shape/{}", shapeId.value));
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────

TessellationOutput tessellateBooleanShape(const SemanticShapeVariant &shape,
                                          const SemanticScene &scene,
                                          const ITessellator &primitiveTessellator,
                                          const TessellationParams &params) {
    TessellationOutput result;
    result.succeeded = false; // only overwritten when an actual boolean is processed

    // Extract the boolean shape and resolve recursively.
    auto processBoolean = [&](const auto &boolShape) -> TessellationOutput {
        TessellationOutput out;
        auto left =
            resolveAndTessellate(boolShape.left, scene, primitiveTessellator, params, out.diags, 0);
        if (!left) {
            out.succeeded = false;
            return out;
        }
        auto right = resolveAndTessellate(boolShape.right, scene, primitiveTessellator, params,
                                          out.diags, 0);
        if (!right) {
            out.succeeded = false;
            return out;
        }

        if (boolShape.rightTransform != glm::dmat4{1.0}) {
            *right = right->Transform(toManifoldTransform(boolShape.rightTransform));
        }

        try {
            manifold::Manifold combined;
            using T = std::decay_t<decltype(boolShape)>;
            if constexpr (std::is_same_v<T, BooleanUnion>) {
                combined = *left + *right;
            } else if constexpr (std::is_same_v<T, BooleanSubtraction>) {
                combined = *left - *right;
            } else if constexpr (std::is_same_v<T, BooleanIntersection>) {
                combined = *left ^ *right;
            }

            if (combined.Status() != manifold::Manifold::Error::NoError) {
                out.diags.warn(codes::kWarnTessBooleanManifoldFail,
                               std::format("boolean operation failed (status {})",
                                           static_cast<int>(combined.Status())));
                out.succeeded = false;
                return out;
            }

            // Carry forward operand diagnostics; manifoldToMesh starts fresh.
            auto meshOut = manifoldToMesh(combined);
            meshOut.diags = std::move(out.diags);
            return meshOut;
        } catch (const std::exception &e) {
            out.diags.warn(codes::kWarnTessBooleanManifoldFail,
                           std::format("boolean operation threw: {}", e.what()));
            out.succeeded = false;
            return out;
        }
    };

    using detail::overloaded;
    return std::visit(overloaded{
                          [&](const BooleanUnion &s) { return processBoolean(s); },
                          [&](const BooleanSubtraction &s) { return processBoolean(s); },
                          [&](const BooleanIntersection &s) { return processBoolean(s); },
                          [&](const auto &) -> TessellationOutput { return result; },
                      },
                      shape);
}

} // namespace nodehammer
