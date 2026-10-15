#include "../cli/cli_test_support.hpp"
#include <G4Box.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4Material.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4SolidStore.hh>
#include <G4StateManager.hh>
#include <G4ios.hh>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/geant4/semantic/importer.hpp>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace nodehammer;
using namespace nodehammer::ir;
using Catch::Approx;
namespace {
const auto fixture = std::filesystem::path{NODEHAMMER_FIXTURES_DIR} / "gdml/shared.gdml";
}
TEST_CASE("GDML: registry, units, shared placements and copy numbers", "[import][gdml]") {
    auto registry = ImporterRegistry::makeDefault();
    REQUIRE(registry.resolve(fixture));
    REQUIRE(registry.resolve("geometry.GDML")->formatName() == "gdml");
    auto result = registry.resolve(fixture)->import(fixture);
    const auto &scene = result.scene;
    REQUIRE(scene.validate() == 5);
    REQUIRE(scene.logVols.size() == 4);
    const auto &world = scene.logVols.at(scene.root.logVolId);
    const auto box = std::get<semantic::BoxShape>(scene.shapes.at(world.shapeId).data);
    REQUIRE(box.dx == Approx(100));
    REQUIRE(world.daughters[0].logVolId == world.daughters[1].logVolId);
    const auto &child = scene.logVols.at(world.daughters[0].logVolId);
    const auto childBox = std::get<semantic::BoxShape>(scene.shapes.at(child.shapeId).data);
    REQUIRE(childBox.dx == Approx(1));
    REQUIRE(childBox.dy == Approx(2));
    REQUIRE(childBox.dz == Approx(3));
    REQUIRE(scene.materials.at(child.materialId).density == Approx(0.25));
    const auto &matrix = world.daughters[0].localTransform;
    REQUIRE(matrix[3][0] == Approx(10));
    REQUIRE(matrix[3][1] == Approx(20));
    REQUIRE(matrix[3][2] == Approx(30));
    // GDML rotation is a frame rotation; the object rotates in the opposite direction.
    REQUIRE(matrix[0][1] == Approx(-1));
    REQUIRE(matrix[1][0] == Approx(1));
    REQUIRE(scene.metadata.tagSets.at(scene.metadata.placementTags.at({world.id, 0}))
                .at("copyNumber") == "7");
    REQUIRE(scene.metadata.tagSets.at(scene.metadata.placementTags.at({world.id, 1}))
                .at("copyNumber") == "8");
}
TEST_CASE("GDML: boolean displacement and Geant4 fallback facets", "[import][gdml]") {
    auto result = GdmlImporter{}.import(fixture);
    const auto &scene = result.scene;
    const auto &world = scene.logVols.at(scene.root.logVolId);
    const auto &cut = scene.logVols.at(world.daughters[2].logVolId);
    const auto &boolean = std::get<semantic::BooleanSubtraction>(scene.shapes.at(cut.shapeId).data);
    REQUIRE(boolean.rightTransform[3][0] == Approx(0.3));
    REQUIRE(boolean.rightTransform[0][2] == Approx(-1));
    REQUIRE(boolean.rightTransform[2][0] == Approx(1));
    const auto &mesh = scene.logVols.at(world.daughters[3].logVolId);
    const auto &triangles =
        std::get<semantic::TessellatedShape>(scene.shapes.at(mesh.shapeId).data).triangles;
    REQUIRE(triangles.size() == 12);
    double volume = 0;
    for (const auto &t : triangles)
        volume += glm::dot(t.vertices[0], glm::cross(t.vertices[1], t.vertices[2])) / 6;
    REQUIRE(volume == Approx(8));
}
TEST_CASE("GDML: repeated imports restore stores, streams and exception handler",
          "[import][gdml]") {
    auto host = std::make_unique<G4Box>("hostOwned", 1, 2, 3);
    const auto solidCount = G4SolidStore::GetInstance()->size();
    const auto volumeCount = G4LogicalVolumeStore::GetInstance()->size();
    const auto placementCount = G4PhysicalVolumeStore::GetInstance()->size();
    auto *handler = G4StateManager::GetStateManager()->GetExceptionHandler();
    auto *stream = G4cout.rdbuf();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(GdmlImporter{}.import(fixture).scene.validate() == 5);
        REQUIRE(G4SolidStore::GetInstance()->size() == solidCount);
        REQUIRE(G4LogicalVolumeStore::GetInstance()->size() == volumeCount);
        REQUIRE(G4PhysicalVolumeStore::GetInstance()->size() == placementCount);
        REQUIRE(G4StateManager::GetStateManager()->GetExceptionHandler() == handler);
        REQUIRE(G4cout.rdbuf() == stream);
        REQUIRE(host->GetXHalfLength() == 1);
    }
    REQUIRE_THROWS_AS(GdmlImporter{}.import(fixture.parent_path() / "invalid.gdml"), Error);
    REQUIRE(G4SolidStore::GetInstance()->size() == solidCount);
    REQUIRE(G4StateManager::GetStateManager()->GetExceptionHandler() == handler);
    REQUIRE(G4cout.rdbuf() == stream);
    REQUIRE(GdmlImporter{}.import(fixture).scene.validate() == 5);
    REQUIRE_THROWS_AS(GdmlImporter{}.import("missing.gdml"), Error);
}

TEST_CASE("GDML: JSON stdout and NHB roundtrip", "[import][gdml][cli]") {
    const auto path = fixture.string();
    const auto outcome =
        nhtest::runCaptured({"inspect", "--output-format", "json", "summary", "-i", path});
    REQUIRE(outcome.code == 0);
    const auto report = nlohmann::json::parse(outcome.out);
    REQUIRE(report.at("nodes") == 5);
    REQUIRE(report.at("format") == "gdml");
    const auto scene = GdmlImporter{}.import(fixture).scene;
    const auto roundtrip = semanticSceneFromBytes(semanticSceneToBytes(scene));
    REQUIRE(roundtrip.validate() == scene.validate());
    REQUIRE(roundtrip.metadata.placementTags == scene.metadata.placementTags);
    REQUIRE(roundtrip.metadata.tagSets == scene.metadata.tagSets);
}

TEST_CASE("GDML: replicas fail explicitly rather than losing copies", "[import][gdml]") {
    try {
        (void)GdmlImporter{}.import(fixture.parent_path() / "replica.gdml");
        FAIL("replicas must not silently import as a single placement");
    } catch (const Error &error) {
        REQUIRE(std::string{error.what()}.find("replica and parameterised") != std::string::npos);
    }
    REQUIRE(GdmlImporter{}.import(fixture).scene.validate() == 5);
}

TEST_CASE("GDML: later files resolve their own material definitions", "[import][gdml]") {
    (void)GdmlImporter{}.import(fixture);
    auto *previousMaterial = G4Material::GetMaterial("TestMaterial", false);
    REQUIRE(previousMaterial != nullptr);
    const auto previousName = previousMaterial->GetName();
    const auto previousDensity = previousMaterial->GetDensity();
    std::ifstream input(fixture);
    std::string xml{std::istreambuf_iterator<char>{input}, {}};
    xml.replace(xml.find("0.25"), 4, "0.75");
    const auto path = std::filesystem::temp_directory_path() /
                      ("nh_gdml_material_" + std::to_string(nhtest::currentProcessId()) + ".gdml");
    struct Remove {
        std::filesystem::path path;
        ~Remove() { std::filesystem::remove(path); }
    } remove{path};
    {
        std::ofstream output(path);
        output << xml;
    }
    const auto result = GdmlImporter{}.import(path);
    REQUIRE(result.scene.materials.begin()->second.density == Approx(0.75));
    REQUIRE(result.scene.materials.begin()->second.name == "TestMaterial");
    REQUIRE(previousMaterial->GetName() == previousName);
    REQUIRE(previousMaterial->GetDensity() == previousDensity);
}
