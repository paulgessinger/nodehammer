// Integration tests: TGeo shape → dispatchTGeoShape → PrimitiveTessellator.
// Each test builds a TGeoManager programmatically, dispatches the shape to our
// internal SemanticShapeVariant, tessellates it, and writes an OBJ file that
// can be opened in Blender or MeshLab for visual inspection.
//
// OBJ files are written to NODEHAMMER_TESS_OBJ_DIR (set by CMake).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/ir/tgeo/semantic/shape_dispatch.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>

#include <TGeoBBox.h>
#include <TGeoCone.h>
#include <TGeoManager.h>
#include <TGeoMaterial.h>
#include <TGeoMatrix.h>
#include <TGeoPcon.h>
#include <TGeoPgon.h>
#include <TGeoTessellated.h>
#include <TGeoTorus.h>
#include <TGeoTrd2.h>
#include <TGeoTube.h>
#include <TGeoVolume.h>

#include "test_obj_writer.hpp"

#include <glm/glm.hpp>

using namespace nodehammer;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void resetManager() {
    delete gGeoManager;
    gGeoManager = nullptr;
}

/// Dispatch the shape of `vol`'s first daughter (or top volume itself if
/// noChildren) and tessellate it. Writes an OBJ to objName in the output dir.
struct DispatchResult {
    TessellationOutput tess;
    DiagnosticList dispatchDiags;
};

static DispatchResult dispatchAndTessellate(const TGeoShape *shape, const std::string &objName,
                                            const TessellationParams &params = {}) {
    SemanticScene scene;
    DiagnosticList diags;
    const SemanticShapeId shapeId = dispatchTGeoShape(shape, scene, diags);

    PrimitiveTessellator tess;
    const SemanticShapeVariant &variant = scene.shapes.at(shapeId).data;
    TessellationOutput out = tess.tessellate(variant, params);
    test::writeObjToDir(out, objName);
    return {std::move(out), std::move(diags)};
}

static bool hasNoDegenerateTriangles(const TessellationOutput &out) {
    for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3) {
        const auto &v0 = out.vertices[out.indices[i + 0]].position;
        const auto &v1 = out.vertices[out.indices[i + 1]].position;
        const auto &v2 = out.vertices[out.indices[i + 2]].position;
        if (glm::length(glm::cross(v1 - v0, v2 - v0)) < 1e-10f)
            return false;
    }
    return true;
}

// ── TGeoBBox ──────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoBBox dispatches to BoxShape and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_box", "tgeo_box");
    TGeoBBox shape{"box", 5.0, 10.0, 15.0};

    auto [out, diags] = dispatchAndTessellate(&shape, "box");

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.vertices.size() == 24);
    REQUIRE(out.indices.size() == 36);
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TGeoTube ──────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoTube (solid) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_tube_solid", "tgeo_tube_solid");
    TGeoTube shape{"tube_solid", 0.0, 3.0, 10.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "tube_solid", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("TGeo->Tess: TGeoTube (hollow) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_tube_hollow", "tgeo_tube_hollow");
    TGeoTube shape{"tube_hollow", 1.5, 3.0, 10.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "tube_hollow", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("TGeo->Tess: TGeoTubeSeg (partial phi) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_tubeseg", "tgeo_tubeseg");
    TGeoTubeSeg shape{"tubeseg", 1.0, 3.0, 10.0, 0.0, 270.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "tubeseg", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TGeoCone ──────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoCone (solid frustum) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_cone_frustum", "tgeo_cone_frustum");
    // rmin1=0, rmax1=2, rmin2=0, rmax2=4, dz=5
    TGeoCone shape{"cone_frustum", 5.0, 0.0, 2.0, 0.0, 4.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "cone_frustum", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("TGeo->Tess: TGeoCone (hollow frustum, rMin>0) dispatches and tessellates",
          "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_cone_hollow", "tgeo_cone_hollow");
    // rmin1=0.5, rmax1=2.0, rmin2=1.0, rmax2=4.0, dz=5 — both inner and outer walls taper
    TGeoCone shape{"cone_hollow", 5.0, 0.5, 2.0, 1.0, 4.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "cone_hollow", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
    // Hollow frustum: outer wall + inner wall + bottom cap (annular) + top cap (annular)
}

TEST_CASE("TGeo->Tess: TGeoCone (apex) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_cone_apex", "tgeo_cone_apex");
    // rmin1=0, rmax1=3, rmin2=0, rmax2=0, dz=5  → pointed tip at +z
    TGeoCone shape{"cone_apex", 5.0, 0.0, 3.0, 0.0, 0.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "cone_apex", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TGeoTrd2 ──────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoTrd2 dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_trd2", "tgeo_trd2");
    // dx1=2, dx2=4, dy1=1, dy2=3, dz=5
    TGeoTrd2 shape{"trd2", 2.0, 4.0, 1.0, 3.0, 5.0};

    auto [out, diags] = dispatchAndTessellate(&shape, "trd2");

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.vertices.size() == 24);
    REQUIRE(out.indices.size() == 36);
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TGeoTorus ─────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoTorus (solid) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_torus_solid", "tgeo_torus_solid");
    // rmin=0, rmax=0.5, rtor=3, phi1=0, dphi=360
    TGeoTorus shape{"torus_solid", 3.0, 0.0, 0.5, 0.0, 360.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "torus_solid", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("TGeo->Tess: TGeoTorus (hollow annular) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_torus_hollow", "tgeo_torus_hollow");
    // rmin=0.2, rmax=0.5, rtor=3, phi1=0, dphi=360  → hollow tube cross-section
    TGeoTorus shape{"torus_hollow", 3.0, 0.2, 0.5, 0.0, 360.0};

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "torus_hollow", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TGeoPcon ──────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoPcon (solid, stepped) dispatches and tessellates", "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_pcon_solid", "tgeo_pcon_solid");
    // 3-section polycone: narrower at both ends, wider in the middle (rMin=0 throughout)
    TGeoPcon shape{"pcon_solid", 0.0, 360.0, 3};
    shape.DefineSection(0, -10.0, 0.0, 2.0);
    shape.DefineSection(1, 0.0, 0.0, 4.0);
    shape.DefineSection(2, 10.0, 0.0, 2.0);

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "pcon_solid", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("TGeo->Tess: TGeoPcon (hollow, constant radii) dispatches and tessellates",
          "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_pcon_hollow", "tgeo_pcon_hollow");
    // 2-section Pcon with rMin>0: an annular cylinder expressed as a polycone.
    // Exercises inner wall + annular ring end caps.
    TGeoPcon shape{"pcon_hollow", 0.0, 360.0, 2};
    shape.DefineSection(0, -8.0, 1.0, 3.0);
    shape.DefineSection(1, 8.0, 1.0, 3.0);

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "pcon_hollow", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

TEST_CASE("TGeo->Tess: TGeoPcon (varying rMin, flanged beampipe) dispatches and tessellates",
          "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_pcon_flange", "tgeo_pcon_flange");
    // 4-section Pcon: thin beampipe with a thicker flange in the middle.
    // rMin varies between sections, exercising the inner wall transition.
    TGeoPcon shape{"pcon_flange", 0.0, 360.0, 4};
    shape.DefineSection(0, -10.0, 0.5, 1.0); // thin pipe
    shape.DefineSection(1, -2.0, 0.5, 1.0);  // thin pipe continues
    shape.DefineSection(2, -2.0, 0.5, 3.0);  // flange begins (rMax jumps)
    shape.DefineSection(3, 2.0, 0.5, 3.0);   // flange ends

    TessellationParams p;
    p.maxSegmentsCircle = 16;
    auto [out, diags] = dispatchAndTessellate(&shape, "pcon_flange", p);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
}

// ── TGeoPgon ──────────────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoPgon (solid hexagonal prism) dispatches and tessellates",
          "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_pgon_solid", "tgeo_pgon_solid");
    // 6-sided polygon, 2 sections, rMin=0 → solid prism with hexagonal caps
    TGeoPgon shape{"pgon_solid", 0.0, 360.0, 6, 2};
    shape.DefineSection(0, -5.0, 0.0, 3.0);
    shape.DefineSection(1, 5.0, 0.0, 3.0);

    auto [out, diags] = dispatchAndTessellate(&shape, "pgon_solid");

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
    // 6 outer-wall quads + 6 bottom-cap tris + 6 top-cap tris = 24 triangles, 60 vertices
    REQUIRE(out.vertices.size() == 60);
    REQUIRE(out.indices.size() == 72); // 24 triangles × 3
}

TEST_CASE("TGeo->Tess: TGeoPgon (hollow hexagonal prism) dispatches and tessellates",
          "[tgeo][tess]") {
    resetManager();
    new TGeoManager("tgeo_pgon_hollow", "tgeo_pgon_hollow");
    // 6-sided polygon, rMin=1.5 → hollow prism with annular hexagonal caps
    TGeoPgon shape{"pgon_hollow", 0.0, 360.0, 6, 2};
    shape.DefineSection(0, -5.0, 1.5, 3.0);
    shape.DefineSection(1, 5.0, 1.5, 3.0);

    auto [out, diags] = dispatchAndTessellate(&shape, "pgon_hollow");

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE_FALSE(out.vertices.empty());
    REQUIRE(hasNoDegenerateTriangles(out));
    // 6 outer + 6 inner + 6 bottom-cap quads + 6 top-cap quads = 24 quads = 48 tris, 96 verts
    REQUIRE(out.vertices.size() == 96);
    REQUIRE(out.indices.size() == 144); // 48 triangles × 3
}

// ── TGeoTessellated ───────────────────────────────────────────────────────────

TEST_CASE("TGeo->Tess: TGeoTessellated (tetrahedron) dispatches and tessellates", "[tgeo][tess]") {
    using Vertex_t = TGeoTessellated::Vertex_t;

    resetManager();
    new TGeoManager("tgeo_tessellated", "tgeo_tessellated");
    auto *shape = new TGeoTessellated("tetra");
    // A regular tetrahedron with 4 triangular facets
    shape->AddFacet(Vertex_t{0, 0, 0}, Vertex_t{1, 0, 0}, Vertex_t{0, 1, 0});
    shape->AddFacet(Vertex_t{0, 0, 0}, Vertex_t{0, 1, 0}, Vertex_t{0, 0, 1});
    shape->AddFacet(Vertex_t{0, 0, 0}, Vertex_t{0, 0, 1}, Vertex_t{1, 0, 0});
    shape->AddFacet(Vertex_t{1, 0, 0}, Vertex_t{0, 0, 1}, Vertex_t{0, 1, 0});
    shape->CloseShape(false, false, false);

    auto [out, diags] = dispatchAndTessellate(shape, "tessellated");

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE_FALSE(out.diags.hasErrors());
    REQUIRE(out.vertices.size() == 12); // 4 facets × 3 verts
    REQUIRE(out.indices.size() == 12);
    REQUIRE(hasNoDegenerateTriangles(out));
}
