#include <catch2/catch_test_macros.hpp>

#include <ir/diagnostics.hpp>
#include <ir/semantic.hpp>
#include <tessellation/boolean_tessellator.hpp>
#include <tessellation/primitive_tessellator.hpp>

#include <manifold/manifold.h>

#include <numbers>

using namespace nodehammer;

namespace {
const TessellationParams kDefault;
constexpr double kPi = std::numbers::pi;
} // namespace

// A full-phi hollow polyhedron (the shape used for the ODD calorimeter
// envelopes) must tessellate to a watertight, manifold mesh. The φ=2π seam is
// the trap: sin/cos(2π) in float lands ~2.6e-5 off the start vertex, so without
// snapping the wrap angle to φ0 the seam never welds and Manifold rejects the
// mesh as non-manifold — which silently skips the angle-cut boolean on the calo.
TEST_CASE("boolean tessellator: full hollow pgon is manifold",
          "[tessellation][boolean][manifold]") {
    PrimitiveTessellator tess;
    PgonShape pgon;
    pgon.phiStart = 0.0;
    pgon.phiDelta = 2.0 * kPi;
    pgon.nSides = 16;
    pgon.sections = {{-12.0, 31.5, 147.0}, {12.0, 31.5, 147.0}}; // {z, rMin, rMax}

    auto mesh = tess.tessellate(SemanticShapeVariant{pgon}, kDefault);
    REQUIRE_FALSE(mesh.vertices.empty());

    DiagnosticList diags;
    auto m = meshToManifold(mesh, diags, "pgon");
    REQUIRE(m.has_value());
    CHECK(m->Status() == manifold::Manifold::Error::NoError);
    CHECK(m->Volume() > 0.0);
}

// Same check for a calorimeter-scale envelope: coordinates beyond ~200 mm used
// to overflow the 32-bit weld key, corrupting the vertex merge. Guards both the
// seam snap and the 64-bit weld key.
TEST_CASE("boolean tessellator: large full hollow pgon is manifold",
          "[tessellation][boolean][manifold]") {
    PrimitiveTessellator tess;
    PgonShape pgon;
    pgon.phiStart = 0.0;
    pgon.phiDelta = 2.0 * kPi;
    pgon.nSides = 16;
    pgon.sections = {{-345.0, 160.0, 343.6}, {345.0, 160.0, 343.6}};

    auto mesh = tess.tessellate(SemanticShapeVariant{pgon}, kDefault);
    DiagnosticList diags;
    auto m = meshToManifold(mesh, diags, "pgon_large");
    REQUIRE(m.has_value());
    CHECK(m->Status() == manifold::Manifold::Error::NoError);
    CHECK(m->Volume() > 0.0);
}

// A partial-phi tube subtracted from a box must remove the sector it names. The
// wedge for [0°,90°] is the +x/+y quadrant; subtracting it from a box centred on
// the origin must clear that quadrant (and only that quadrant). A 180° error in
// the wedge construction would instead clear the -x/-y quadrant.
TEST_CASE("boolean tessellator: partial tube subtraction clears the named sector",
          "[tessellation][boolean][manifold]") {
    SemanticScene scene;
    const SemanticShapeId boxId = scene.nextShapeId();
    scene.shapes[boxId] = {boxId, BoxShape{4.0, 4.0, 1.0}};
    const SemanticShapeId wedgeId = scene.nextShapeId();
    // rMin=0, rMax oversized to fully clear the box corner, sector [0°,90°].
    scene.shapes[wedgeId] = {wedgeId, TubeShape{0.0, 20.0, 5.0, 0.0, kPi / 2.0}};
    const SemanticShapeId cutId = scene.nextShapeId();
    scene.shapes[cutId] = {cutId, BooleanSubtraction{boxId, wedgeId, glm::dmat4{1.0}}};

    PrimitiveTessellator tess;
    auto out = tessellateBooleanShape(scene.shapes.at(cutId).data, scene, tess, kDefault);
    REQUIRE(out.succeeded);
    REQUIRE_FALSE(out.vertices.empty());

    // No surviving vertex sits strictly inside the removed quadrant.
    bool anyInRemoved = false;
    bool anyInOpposite = false;
    for (const auto &v : out.vertices) {
        const float x = v.position.x, y = v.position.y;
        if (x > 0.1f && y > 0.1f) {
            anyInRemoved = true;
        }
        if (x < -0.1f && y < -0.1f) {
            anyInOpposite = true;
        }
    }
    CHECK_FALSE(anyInRemoved); // first quadrant cleared
    CHECK(anyInOpposite);      // opposite quadrant retained
}
