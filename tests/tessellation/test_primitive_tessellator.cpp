#include <catch2/catch_test_macros.hpp>
#include <ir/semantic.hpp>
#include <tessellation/primitive_tessellator.hpp>

#include <glm/glm.hpp>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::tessellation;

static const TessellationParams kDefault;

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool hasNoDegenerateTriangles(const TessellationOutput &out) {
    for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3) {
        const auto &v0 = out.vertices[out.indices[i + 0]].position;
        const auto &v1 = out.vertices[out.indices[i + 1]].position;
        const auto &v2 = out.vertices[out.indices[i + 2]].position;
        const glm::vec3 cross = glm::cross(v1 - v0, v2 - v0);
        if (glm::length(cross) < 1e-10f)
            return false;
    }
    return true;
}

static bool normalsAreUnitLength(const TessellationOutput &out) {
    for (const auto &v : out.vertices) {
        if (std::abs(glm::length(v.normal) - 1.0f) > 1e-4f)
            return false;
    }
    return true;
}

// ── Box ───────────────────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: box has 24 vertices and 36 indices", "[tessellation][box]") {
    PrimitiveTessellator t;
    ir::semantic::BoxShape s{1.0, 1.0, 1.0};
    auto out = t.tessellate(s, kDefault);

    REQUIRE(out.vertices.size() == 24);
    REQUIRE(out.indices.size() == 36);
    REQUIRE(out.diags.empty());
}

TEST_CASE("PrimitiveTessellator: box normals are unit length", "[tessellation][box]") {
    PrimitiveTessellator t;
    ir::semantic::BoxShape s{2.0, 3.0, 4.0};
    auto out = t.tessellate(s, kDefault);
    REQUIRE(normalsAreUnitLength(out));
}

TEST_CASE("PrimitiveTessellator: box has no degenerate triangles", "[tessellation][box]") {
    PrimitiveTessellator t;
    ir::semantic::BoxShape s{1.0, 2.0, 0.5};
    auto out = t.tessellate(s, kDefault);
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("PrimitiveTessellator: box indices stay within vertex range", "[tessellation][box]") {
    PrimitiveTessellator t;
    ir::semantic::BoxShape s{1.0, 1.0, 1.0};
    auto out = t.tessellate(s, kDefault);
    for (const auto idx : out.indices) {
        REQUIRE(idx < out.vertices.size());
    }
}

// ── Tube ──────────────────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: tube vertex count for N segments (solid)",
          "[tessellation][tube]") {
    PrimitiveTessellator t;
    ir::semantic::TubeShape s{0.0, 1.0, 2.0}; // rMin=0 → solid tube
    TessellationParams p;
    p.maxSegmentsCircle = 8;
    auto out = t.tessellate(s, p);

    // Outer wall: 8 quads × 4 verts = 32
    // +Z cap:     8 tris × 3 verts  = 24
    // -Z cap:     8 tris × 3 verts  = 24
    // Total: 80
    REQUIRE(out.vertices.size() == 80);
    REQUIRE(out.diags.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("PrimitiveTessellator: hollow tube has inner wall", "[tessellation][tube]") {
    PrimitiveTessellator t;
    ir::semantic::TubeShape s{0.5, 1.0, 2.0}; // hollow
    TessellationParams p;
    p.maxSegmentsCircle = 8;
    auto out = t.tessellate(s, p);

    // Outer wall: 8 quads × 4 = 32
    // Inner wall: 8 quads × 4 = 32
    // +Z cap:     8 quads × 4 = 32
    // -Z cap:     8 quads × 4 = 32
    REQUIRE(out.vertices.size() == 128);
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── Cone ──────────────────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: cone (solid apex) no degenerate triangles",
          "[tessellation][cone]") {
    PrimitiveTessellator t;
    ir::semantic::ConeShape s{0, 2.0, 0, 0, 3.0}; // solid apex at top
    TessellationParams p;
    p.maxSegmentsCircle = 12;
    auto out = t.tessellate(s, p);
    REQUIRE(out.diags.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── Torus ─────────────────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: torus vertex count matches double-parametric formula",
          "[tessellation][torus]") {
    PrimitiveTessellator t;
    ir::semantic::TorusShape s{0.0, 0.2, 1.0}; // rMin=0, rMax=0.2, rTor=1.0
    TessellationParams p;
    p.maxSegmentsCircle = 8;
    auto out = t.tessellate(s, p);

    // nTube = max(3, 8/2) = 4; segs = 8
    // segs * nTube quads × 4 verts each = 8 * 4 * 4 = 128
    const int segs = 8;
    const int nTube = std::max(3, segs / 2);
    REQUIRE(out.vertices.size() == static_cast<std::size_t>(segs * nTube * 4));
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TessellatedShape ──────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: TessellatedShape passes through triangles",
          "[tessellation][tessellated]") {
    PrimitiveTessellator t;
    ir::semantic::TessellatedShape s;
    s.triangles.push_back({glm::dvec3{0, 0, 0}, glm::dvec3{1, 0, 0}, glm::dvec3{0, 1, 0}});
    s.triangles.push_back({glm::dvec3{0, 0, 0}, glm::dvec3{0, 1, 0}, glm::dvec3{0, 0, 1}});

    auto out = t.tessellate(s, kDefault);
    REQUIRE(out.vertices.size() == 6);
    REQUIRE(out.indices.size() == 6);
    REQUIRE(out.diags.empty());
    REQUIRE(normalsAreUnitLength(out));
}

// ── UnknownShape ──────────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: UnknownShape produces empty mesh and error diagnostic",
          "[tessellation][unknown]") {
    PrimitiveTessellator t;
    ir::semantic::UnknownShape s{"MyCustomSolid"};
    auto out = t.tessellate(s, kDefault);

    REQUIRE(out.vertices.empty());
    REQUIRE(out.indices.empty());
    REQUIRE(out.diags.hasErrors());
}

// ── canTessellate ─────────────────────────────────────────────────────────────

TEST_CASE("PrimitiveTessellator: canTessellate returns false for boolean variants",
          "[tessellation]") {
    PrimitiveTessellator t;

    REQUIRE_FALSE(t.canTessellate(
        ir::semantic::BooleanUnion{ir::semantic::ShapeId{1}, ir::semantic::ShapeId{2}}));
    REQUIRE_FALSE(t.canTessellate(
        ir::semantic::BooleanIntersection{ir::semantic::ShapeId{1}, ir::semantic::ShapeId{2}}));
    REQUIRE_FALSE(t.canTessellate(
        ir::semantic::BooleanSubtraction{ir::semantic::ShapeId{1}, ir::semantic::ShapeId{2}}));
}

TEST_CASE("PrimitiveTessellator: canTessellate returns true for primitive variants",
          "[tessellation]") {
    PrimitiveTessellator t;

    REQUIRE(t.canTessellate(ir::semantic::BoxShape{1, 1, 1}));
    REQUIRE(t.canTessellate(ir::semantic::TubeShape{0, 1, 1}));
    REQUIRE(t.canTessellate(ir::semantic::UnknownShape{"foo"}));
}
