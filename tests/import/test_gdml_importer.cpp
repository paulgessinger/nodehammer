#include "../cli/cli_test_support.hpp"
#include <G4Box.hh>
#include <G4GDMLParser.hh>
#include <G4LogicalVolume.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4Material.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4SolidStore.hh>
#include <G4StateManager.hh>
#include <G4SystemOfUnits.hh>
#include <G4VPhysicalVolume.hh>
#include <G4ios.hh>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/geant4/semantic/importer.hpp>
#include <manifold/manifold.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>
#include <tessellation/boolean_tessellator.hpp>
#include <tessellation/primitive_tessellator.hpp>

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
TEST_CASE("GDML: boolean displacement and semantic extrusion", "[import][gdml]") {
    auto result = GdmlImporter{}.import(fixture);
    const auto &scene = result.scene;
    const auto &world = scene.logVols.at(scene.root.logVolId);
    const auto &cut = scene.logVols.at(world.daughters[2].logVolId);
    const auto &boolean = std::get<semantic::BooleanSubtraction>(scene.shapes.at(cut.shapeId).data);
    REQUIRE(boolean.rightTransform[3][0] == Approx(0.3));
    REQUIRE(boolean.rightTransform[0][2] == Approx(-1));
    REQUIRE(boolean.rightTransform[2][0] == Approx(1));
    const auto &mesh = scene.logVols.at(world.daughters[3].logVolId);
    const auto &shape = scene.shapes.at(mesh.shapeId).data;
    REQUIRE(std::holds_alternative<semantic::ExtrudedShape>(shape));
    const auto output = tessellation::PrimitiveTessellator{}.tessellate(shape, {});
    REQUIRE(output.indices.size() == 36);
    double volume = 0;
    for (size_t i = 0; i < output.indices.size(); i += 3) {
        const glm::dvec3 a = output.vertices[output.indices[i]].position;
        const glm::dvec3 b = output.vertices[output.indices[i + 1]].position;
        const glm::dvec3 c = output.vertices[output.indices[i + 2]].position;
        volume += glm::dot(a, glm::cross(b, c)) / 6;
    }
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

TEST_CASE("GDML: parametric meshes agree with Geant4 solids", "[import][gdml][parametric]") {
    const auto path = fixture.parent_path() / "parametric.gdml";
    auto result = GdmlImporter{}.import(path);
    G4GDMLParser reference;
    reference.SetStripFlag(false);
    reference.Read(path.string(), false);
    auto *world = reference.GetWorldVolume()->GetLogicalVolume();
    const auto &scene = result.scene;
    const auto &importedWorld = scene.logVols.at(scene.root.logVolId);
    REQUIRE(importedWorld.daughters.size() == static_cast<size_t>(world->GetNoDaughters()));
    for (size_t child = 0; child < world->GetNoDaughters(); ++child) {
        auto *solid = world->GetDaughter(child)->GetLogicalVolume()->GetSolid();
        INFO(solid->GetName());
        const auto &lv =
            scene.logVols.at(importedWorld.daughters[static_cast<size_t>(child)].logVolId);
        const auto &shape = scene.shapes.at(lv.shapeId).data;
        const std::map<std::string, size_t> types{
            {"para", semantic::ShapeVariant{semantic::ParaShape{}}.index()},
            {"torus", semantic::ShapeVariant{semantic::TorusShape{}}.index()},
            {"pgon", semantic::ShapeVariant{semantic::PgonShape{}}.index()},
            {"pgonFull", semantic::ShapeVariant{semantic::PgonShape{}}.index()},
            {"pgonTip", semantic::ShapeVariant{semantic::PgonShape{}}.index()},
            {"concave", semantic::ShapeVariant{semantic::ExtrudedShape{}}.index()},
            {"twisted", semantic::ShapeVariant{semantic::GenericTrapShape{}}.index()},
            {"triangular", semantic::ShapeVariant{semantic::GenericTrapShape{}}.index()},
            {"trap", semantic::ShapeVariant{semantic::GenericTrapShape{}}.index()}};
        REQUIRE(shape.index() == types.at(solid->GetName()));
        const auto mesh = tessellation::PrimitiveTessellator{}.tessellate(
            shape, {.maxSegmentsCircle = 128, .genericTrapTolerance = 0.0001});
        REQUIRE(mesh.succeeded);
        REQUIRE_FALSE(mesh.indices.empty());
        DiagnosticList diagnostics;
        const auto manifold = tessellation::meshToManifold(mesh, diagnostics, "reference");
        REQUIRE(manifold.has_value());
        REQUIRE(manifold->Status() == manifold::Manifold::Error::NoError);
        double volume = 0, maxSurfaceError = 0;
        using Point = std::array<float, 3>;
        std::map<std::pair<Point, Point>, int> edges;
        auto point = [](glm::vec3 v) { return Point{v.x, v.y, v.z}; };
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            std::array<glm::dvec3, 3> v;
            for (size_t j = 0; j < 3; ++j)
                v[j] = mesh.vertices.at(mesh.indices[i + j]).position;
            const auto normal = glm::cross(v[1] - v[0], v[2] - v[0]);
            REQUIRE(glm::length(normal) > 0);
            for (size_t j = 0; j < 3; ++j)
                REQUIRE(glm::dot(normal, glm::dvec3{mesh.vertices[mesh.indices[i + j]].normal}) >
                        0);
            volume += glm::dot(v[0], glm::cross(v[1], v[2])) / 6;
            const auto centroid = (v[0] + v[1] + v[2]) * (cm / 3);
            const G4ThreeVector sample{centroid.x, centroid.y, centroid.z};
            const double distance = solid->Inside(sample) == kOutside
                                        ? solid->DistanceToIn(sample)
                                        : solid->DistanceToOut(sample);
            maxSurfaceError = std::max(maxSurfaceError, distance / cm);
            for (size_t j = 0; j < 3; ++j) {
                auto a = point(v[j]), b = point(v[(j + 1) % 3]);
                edges[{a, b}]++;
            }
        }
        for (const auto &[edge, count] : edges)
            REQUIRE(count == edges[std::make_pair(edge.second, edge.first)]);
        REQUIRE(volume == Approx(solid->GetCubicVolume() / (cm * cm * cm)).epsilon(0.003));
        REQUIRE(maxSurfaceError < 0.004);
        if (solid->GetName() == "twisted") {
            const auto coarse = tessellation::PrimitiveTessellator{}.tessellate(
                shape, {.maxSegmentsCircle = 4, .genericTrapTolerance = 0.1});
            REQUIRE(mesh.indices.size() > coarse.indices.size());
            double coarseVolume = 0;
            for (size_t i = 0; i < coarse.indices.size(); i += 3) {
                const glm::dvec3 a = coarse.vertices[coarse.indices[i]].position;
                const glm::dvec3 b = coarse.vertices[coarse.indices[i + 1]].position;
                const glm::dvec3 c = coarse.vertices[coarse.indices[i + 2]].position;
                coarseVolume += glm::dot(a, glm::cross(b, c)) / 6;
            }
            const double exact = solid->GetCubicVolume() / (cm * cm * cm);
            REQUIRE(std::abs(volume - exact) < std::abs(coarseVolume - exact));
        }
    }
}

TEST_CASE("GDML: unmapped solids retain the Geant4 mesh fallback", "[import][gdml]") {
    const auto result = GdmlImporter{}.import(fixture.parent_path() / "fallback.gdml");
    const auto &scene = result.scene;
    const auto &world = scene.logVols.at(scene.root.logVolId);
    const auto &lv = scene.logVols.at(world.daughters[3].logVolId);
    const auto &triangles =
        std::get<semantic::TessellatedShape>(scene.shapes.at(lv.shapeId).data).triangles;
    REQUIRE_FALSE(triangles.empty());
    double volume = 0;
    for (const auto &t : triangles)
        volume += glm::dot(t.vertices[0], glm::cross(t.vertices[1], t.vertices[2])) / 6;
    REQUIRE(volume > 3.9);
    REQUIRE(volume < 4.2);
}
