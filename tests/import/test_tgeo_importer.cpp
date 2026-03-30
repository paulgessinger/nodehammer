#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/import/tgeo/tgeo_importer.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <TGeoBBox.h>
#include <TGeoCompositeShape.h>
#include <TGeoManager.h>
#include <TGeoMatrix.h>
#include <TGeoTube.h>
#include <TGeoVolume.h>

// ── Helpers ───────────────────────────────────────────────────────────────────

/// Import from the currently active gGeoManager by writing to a temp file.
static nodehammer::ImportResult importCurrent() {
    const char *tmpFile = "/tmp/nodehammer_tgeo_test.root";
    gGeoManager->Export(tmpFile);
    nodehammer::TGeoImporter imp;
    return imp.import(tmpFile);
}

/// Delete any lingering gGeoManager and start fresh.
static void resetManager() {
    delete gGeoManager;
    gGeoManager = nullptr;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("TGeoImporter: formatName and supportedExtensions", "[import][tgeo]") {
    nodehammer::TGeoImporter imp;
    REQUIRE(imp.formatName() == "tgeo");
    REQUIRE(imp.supportedExtensions() == std::vector<std::string>{".root"});
}

TEST_CASE("TGeoImporter: TGeoBBox -> BoxShape", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("testBox", "testBox");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    auto *top = mgr->MakeBox("world", med, 10.0, 20.0, 30.0);
    mgr->SetTopVolume(top);
    mgr->CloseGeometry();

    auto result = importCurrent();
    REQUIRE_FALSE(result.diags.hasErrors());

    bool found = false;
    for (const auto &[id, s] : result.scene.shapes) {
        if (std::holds_alternative<nodehammer::BoxShape>(s.data)) {
            const auto &bs = std::get<nodehammer::BoxShape>(s.data);
            REQUIRE(bs.dx == Catch::Approx(10.0));
            REQUIRE(bs.dy == Catch::Approx(20.0));
            REQUIRE(bs.dz == Catch::Approx(30.0));
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("TGeoImporter: TGeoTube -> TubeShape", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("testTube", "testTube");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    auto *top = new TGeoVolume("world", new TGeoTube("tube", 5.0, 10.0, 25.0), med);
    mgr->SetTopVolume(top);
    mgr->CloseGeometry();

    auto result = importCurrent();
    REQUIRE_FALSE(result.diags.hasErrors());

    bool found = false;
    for (const auto &[id, s] : result.scene.shapes) {
        if (std::holds_alternative<nodehammer::TubeShape>(s.data)) {
            const auto &ts = std::get<nodehammer::TubeShape>(s.data);
            REQUIRE(ts.rMin == Catch::Approx(5.0));
            REQUIRE(ts.rMax == Catch::Approx(10.0));
            REQUIRE(ts.dz == Catch::Approx(25.0));
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("TGeoImporter: nested volumes -> correct parent-child hierarchy", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("nested", "nested");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    auto *top = mgr->MakeBox("world", med, 500, 500, 500);
    auto *child = mgr->MakeBox("inner", med, 10, 10, 10);
    top->AddNode(child, 1, new TGeoTranslation(0, 0, 100));
    mgr->SetTopVolume(top);
    mgr->CloseGeometry();

    auto result = importCurrent();
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.scene.nodes.size() == 2);

    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(root.children.size() == 1);

    const auto childId = root.children.front();
    const auto &childNode = result.scene.nodes.at(childId);
    REQUIRE(childNode.parentId == result.scene.rootId);
    REQUIRE(childNode.worldTransform[3].z == Catch::Approx(100.0));
}

TEST_CASE("TGeoImporter: same TGeoVolume placed twice -> one LV, two nodes", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("reuse", "reuse");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    auto *top = mgr->MakeBox("world", med, 500, 500, 500);
    auto *child = mgr->MakeBox("brick", med, 10, 10, 10);
    top->AddNode(child, 1, new TGeoTranslation(50, 0, 0));
    top->AddNode(child, 2, new TGeoTranslation(-50, 0, 0));
    mgr->SetTopVolume(top);
    mgr->CloseGeometry();

    auto result = importCurrent();
    REQUIRE(result.scene.nodes.size() == 3);   // world + 2 placements
    REQUIRE(result.scene.logVols.size() == 2); // world LV + brick LV (deduplicated)
}

TEST_CASE("TGeoImporter: TGeoCompositeShape -> BooleanUnion", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("bool", "bool");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    auto *top = mgr->MakeBox("world", med, 500, 500, 500);

    // Constituent shapes are referenced by name from the global shape list
    new TGeoBBox("boolBoxA", 10, 10, 10);
    new TGeoBBox("boolBoxB", 5, 5, 5);
    auto *comp = new TGeoCompositeShape("boolComp", "boolBoxA + boolBoxB");
    auto *compVol = new TGeoVolume("comp_vol", comp, med);
    top->AddNode(compVol, 1);
    mgr->SetTopVolume(top);
    mgr->CloseGeometry();

    auto result = importCurrent();

    bool hasBool = false;
    for (const auto &[id, s] : result.scene.shapes) {
        if (std::holds_alternative<nodehammer::BooleanUnion>(s.data)) {
            hasBool = true;
            const auto &bu = std::get<nodehammer::BooleanUnion>(s.data);
            REQUIRE(result.scene.shapes.contains(bu.left));
            REQUIRE(result.scene.shapes.contains(bu.right));
        }
    }
    REQUIRE(hasBool);
}

TEST_CASE("TGeoImporter: root worldTransform is identity", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("identityTest", "identityTest");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    auto *top = mgr->MakeBox("world", med, 100, 100, 100);
    mgr->SetTopVolume(top);
    mgr->CloseGeometry();

    auto result = importCurrent();
    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(root.worldTransform == glm::dmat4{1.0});
}

TEST_CASE("TGeoImporter: provenance.sourceSystem is tgeo", "[import][tgeo]") {
    resetManager();
    auto *mgr = new TGeoManager("provTest", "provTest");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);
    mgr->SetTopVolume(mgr->MakeBox("world", med, 100, 100, 100));
    mgr->CloseGeometry();

    auto result = importCurrent();
    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(root.provenance.sourceSystem == "tgeo");
}
