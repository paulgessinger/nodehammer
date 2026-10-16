#include <catch2/catch_test_macros.hpp>

#include <diagnostics.hpp>
#include <ir/expanded/scene.hpp>
#include <tessellation/boolean_tessellator.hpp>
#include <tessellation/primitive_tessellator.hpp>

#include <manifold/manifold.h>

#include <array>
#include <cmath>
#include <numbers>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::diagnostics;
using namespace nodehammer::tessellation;

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
    ir::semantic::PgonShape pgon;
    pgon.phiStart = 0.0;
    pgon.phiDelta = 2.0 * kPi;
    pgon.nSides = 16;
    pgon.sections = {{-12.0, 31.5, 147.0}, {12.0, 31.5, 147.0}}; // {z, rMin, rMax}

    auto mesh = tess.tessellate(ir::semantic::ShapeVariant{pgon}, kDefault);
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
    ir::semantic::PgonShape pgon;
    pgon.phiStart = 0.0;
    pgon.phiDelta = 2.0 * kPi;
    pgon.nSides = 16;
    pgon.sections = {{-345.0, 160.0, 343.6}, {345.0, 160.0, 343.6}};

    auto mesh = tess.tessellate(ir::semantic::ShapeVariant{pgon}, kDefault);
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
    ir::expanded::Scene scene;
    const ir::semantic::ShapeId boxId = scene.nextShapeId();
    scene.shapes[boxId] = {boxId, ir::semantic::BoxShape{4.0, 4.0, 1.0}};
    const ir::semantic::ShapeId wedgeId = scene.nextShapeId();
    // rMin=0, rMax oversized to fully clear the box corner, sector [0°,90°].
    scene.shapes[wedgeId] = {wedgeId, ir::semantic::TubeShape{0.0, 20.0, 5.0, 0.0, kPi / 2.0}};
    const ir::semantic::ShapeId cutId = scene.nextShapeId();
    scene.shapes[cutId] = {cutId,
                           ir::semantic::BooleanSubtraction{boxId, wedgeId, glm::dmat4{1.0}}};

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

// IDEA's Ta shields are hollow cone sectors, including a sector crossing 2*pi.
// Rendering alone did not expose their missing radial faces; Boolean cuts did.
TEST_CASE("boolean tessellator: IDEA cone sectors are watertight",
          "[tessellation][boolean][manifold]") {
    PrimitiveTessellator tess;
    const std::array<semantic::ConeShape, 8> shields{{
        {1.989, 2.789, 3.30266, 4.80266, 34.57, 5.689773361501514, 1.1868238913561442},
        {1.666, 1.676, 1.989, 2.789, 8.5, 5.689773361501514, 1.1868238913561442},
        {2.179, 2.879, 3.30266, 4.80266, 29.57, 0.5934119456780721, 0.6283185307179586},
        {2.179, 2.879, 3.30266, 4.80266, 29.57, 5.078908123303498, 0.6108652381980153},
        // Solid sector, both apex orientations, and unchanged full sweep.
        {0, 2, 0, 3, 4, 0, kPi / 2},
        {0, 2, 0, 0, 4, 1, kPi / 2},
        {0, 0, 0, 2, 4, 1, kPi / 2},
        {0, 2, 0, 3, 4, 1, 2 * kPi},
    }};
    for (const auto &shape : shields) {
        for (const int segments : {24, 64}) {
            CAPTURE(shape.phiStart, shape.phiDelta, segments);
            TessellationParams params;
            params.maxSegmentsCircle = segments;
            const auto mesh = tess.tessellate(semantic::ShapeVariant{shape}, params);
            DiagnosticList diags;
            const auto solid = meshToManifold(mesh, diags, "IDEA shield");
            REQUIRE(solid.has_value());
            CHECK(solid->Status() == manifold::Manifold::Error::NoError);
            // Exact volume of the polygonal frustum sector (not the smooth cone).
            const auto radii = [](double a, double b) { return a * a + a * b + b * b; };
            const double expected =
                segments * std::sin(shape.phiDelta / segments) * shape.dz / 3 *
                (radii(shape.rMax1, shape.rMax2) - radii(shape.rMin1, shape.rMin2));
            CHECK(std::abs(solid->Volume() - expected) < expected * 1e-4);
            CHECK_FALSE(diags.hasErrors());
        }
    }
}

// Detector supports can contain long chains of drilled holes. A fixed depth
// cutoff rejected sPHENIX's MVTX end wheel (34 nested boolean operations).
TEST_CASE("boolean tessellator: deep operand graph preserves the cut volume",
          "[tessellation][boolean][manifold]") {
    ir::expanded::Scene scene;
    const auto box = scene.nextShapeId();
    scene.shapes[box] = {box, ir::semantic::BoxShape{10.0, 10.0, 10.0}};
    const auto hole = scene.nextShapeId();
    scene.shapes[hole] = {hole, ir::semantic::BoxShape{1.0, 1.0, 1.0}};
    auto previous = box;
    glm::dmat4 transform{1.0};
    transform[3][0] = 3.0;
    for (int i = 0; i < 128; ++i) {
        const auto id = scene.nextShapeId();
        scene.shapes[id] = {id, ir::semantic::BooleanSubtraction{previous, hole, transform}};
        previous = id;
    }
    // Both sides share the deep operand graph; this is a DAG, not a cycle.
    const auto root = ir::semantic::BooleanUnion{previous, previous, glm::dmat4{1.0}};
    PrimitiveTessellator tess;
    auto out = tessellateBooleanShape(root, scene, tess, kDefault);
    REQUIRE(out.succeeded);
    REQUIRE(out.diags.empty());
    REQUIRE_FALSE(out.vertices.empty());
    DiagnosticList diags;
    auto solid = meshToManifold(out, diags, "deep cut");
    REQUIRE(solid.has_value());
    CHECK(std::abs(solid->Volume() - (8000.0 - 8.0)) < 1e-5);
}

TEST_CASE("boolean tessellator: invalid nested operands do not fall back to primitives",
          "[tessellation][boolean][manifold]") {
    ir::expanded::Scene scene;
    const auto box = scene.nextShapeId();
    scene.shapes[box] = {box, ir::semantic::BoxShape{1.0, 1.0, 1.0}};
    const auto nested = scene.nextShapeId();
    std::string expected;
    SECTION("cycle") {
        scene.shapes[nested] = {nested, ir::semantic::BooleanUnion{box, nested, glm::dmat4{1.0}}};
        expected = "cycle in boolean operands";
    }
    SECTION("missing operand") {
        const auto missing = scene.nextShapeId();
        scene.shapes[nested] = {nested,
                                ir::semantic::BooleanSubtraction{box, missing, glm::dmat4{1.0}}};
        expected = "not found in scene";
    }
    const auto root = ir::semantic::BooleanUnion{nested, box, glm::dmat4{1.0}};
    PrimitiveTessellator tess;
    auto out = tessellateBooleanShape(root, scene, tess, kDefault);
    CHECK_FALSE(out.succeeded);
    CHECK(out.vertices.empty());
    REQUIRE(out.diags.size() == 1);
    CHECK(out.diags.begin()->message.find(expected) != std::string::npos);
}

TEST_CASE("parametric extrusion and trap remain usable boolean operands",
          "[tessellation][boolean][parametric]") {
    std::vector<semantic::ShapeVariant> shapes{
        semantic::ExtrudedShape{{{-2, -1}, {2, -1}, {2, 1}, {-2, 1}},
                                {{-2, {0, 0}, 1}, {2, {0, 0}, 1}}},
        semantic::GenericTrapShape{
            2, {{{-2, -1}, {2, -1}, {2, 1}, {-2, 1}, {-2, -1}, {2, -1}, {2, 1}, {-2, 1}}}}};
    for (const auto &shape : shapes) {
        expanded::Scene scene;
        const semantic::ShapeId left{1}, right{2};
        scene.shapes.emplace(left, semantic::Shape{left, shape});
        scene.shapes.emplace(right, semantic::Shape{right, semantic::BoxShape{1, 2, 3}});
        const auto mesh =
            tessellateBooleanShape(semantic::BooleanSubtraction{left, right, glm::dmat4{1}}, scene,
                                   PrimitiveTessellator{}, {});
        REQUIRE(mesh.succeeded);
        DiagnosticList diags;
        const auto solid = meshToManifold(mesh, diags, "cut");
        REQUIRE(solid.has_value());
        REQUIRE(solid->Status() == manifold::Manifold::Error::NoError);
        REQUIRE(std::abs(solid->Volume() - 16) < 1e-6);
    }
}

TEST_CASE("generic trap supports an apex and triangular endcaps", "[tessellation][parametric]") {
    const semantic::GenericTrapShape pyramid{
        2, {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}}}};
    const auto mesh = PrimitiveTessellator{}.tessellate(pyramid, {});
    DiagnosticList diags;
    const auto solid = meshToManifold(mesh, diags, "pyramid");
    REQUIRE(solid.has_value());
    REQUIRE(solid->Status() == manifold::Manifold::Error::NoError);
    REQUIRE(std::abs(solid->Volume() - 16.0 / 3) < 1e-6);
}

TEST_CASE("boolean tessellator: ATLAS toroid partial polycones are manifold",
          "[tessellation][boolean][manifold]") {
    // ECT_TS_CentralTube_top/low: descending-Z profiles with open phi sweeps.
    for (const auto &[start, sweep] : std::array{std::pair{-0.1221730476396031, 3.385938748868999},
                                                 std::pair{3.490658503988659, 2.443460952792061}}) {
        for (bool descending : {false, true}) {
            ir::semantic::PconShape pcon;
            pcon.phiStart = start;
            pcon.phiDelta = sweep;
            pcon.sections = {{1730.0, 1050.0, 1060.0}, {1990.0, 1305.0, 1315.0}};
            if (descending)
                std::swap(pcon.sections[0], pcon.sections[1]);
            PrimitiveTessellator tess;
            auto mesh = tess.tessellate(ir::semantic::ShapeVariant{pcon}, kDefault);
            DiagnosticList diags;
            auto solid = meshToManifold(mesh, diags, "ECT_TS_CentralTube");
            REQUIRE(solid.has_value());
            CHECK(solid->Status() == manifold::Manifold::Error::NoError);
            CHECK(solid->Volume() > 0.0);
        }
    }
}

TEST_CASE("ATLAS stepped partial polycones have closed radial faces",
          "[tessellation][boolean][manifold]") {
    for (int shape : {15308, 15702, 15703}) {
        CAPTURE(shape);
        semantic::PconShape pcon;
        if (shape == 15308) {
            pcon.phiStart = -0.0698131700797732;
            pcon.phiDelta = 0.1396263401595464;
            pcon.sections = {{3179, 1150, 1217}, {3189, 1150, 1225}, {3189, 1212, 1225},
                             {3405, 1387, 1400}, {3405, 1390, 1400}, {3415, 1390, 1400}};
        } else {
            pcon.phiStart = shape == 15702 ? -0.098174770424681 : -0.0483091787439614;
            pcon.phiDelta = -2 * pcon.phiStart;
            pcon.sections = {{-165, 2044, 2070}, {-10, 2044, 2070}, {-10, 2050.5, 2070},
                             {10, 2050.5, 2070}, {10, 2044, 2070},  {165, 2044, 2070}};
        }
        for (bool descending : {false, true}) {
            if (descending)
                std::reverse(pcon.sections.begin(), pcon.sections.end());
            for (int segments : {24, 48}) {
                auto params = kDefault;
                params.maxSegmentsCircle = segments;
                const auto mesh = PrimitiveTessellator{}.tessellate(pcon, params);
                DiagnosticList diags;
                const auto solid = meshToManifold(mesh, diags, "stepped ATLAS polycone");
                REQUIRE(solid.has_value());
                CHECK(solid->Status() == manifold::Manifold::Error::NoError);
                CHECK(solid->Volume() > 0);
            }
        }
    }
}
