#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ir/semantic.hpp>
#include <ir/semantic/occurrence_tree.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <limits>

using namespace nodehammer::ir::semantic;

namespace {
Scene repeatedGeometry() {
    Scene geometry;
    const ShapeId shape{1};
    const MaterialId material{1};
    geometry.shapes.emplace(shape, Shape{shape, BoxShape{1, 2, 3}});
    geometry.materials.emplace(material, SourceMaterial{material, "Silicon", std::nullopt, 2.33});
    for (uint64_t i = 1; i <= 3; ++i) {
        geometry.logVols.emplace(LogVolId{i},
                                 LogicalVolume{LogVolId{i}, "prototype", shape, material});
    }
    geometry.root = {"world", LogVolId{1}, glm::dmat4{1}};
    geometry.logVols.at(LogVolId{1}).daughters = {
        {"left", LogVolId{2}, glm::translate(glm::dmat4{1}, glm::dvec3{10, 0, 0})},
        {"right", LogVolId{2}, glm::translate(glm::dmat4{1}, glm::dvec3{-10, 0, 0})}};
    geometry.logVols.at(LogVolId{2}).daughters = {
        {"sensor", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{0, 2, 0})},
        {"sensor", LogVolId{3}, glm::translate(glm::dmat4{1}, glm::dvec3{0, 5, 0})}};
    return geometry;
}

Scene binaryLevels(uint64_t levels) {
    auto geometry = repeatedGeometry();
    geometry.logVols.clear();
    for (uint64_t i = 1; i <= levels; ++i) {
        LogicalVolume volume{LogVolId{i}, "level", ShapeId{1}, MaterialId{1}};
        if (i != levels) {
            volume.daughters = {{"left", LogVolId{i + 1}, glm::dmat4{1}},
                                {"right", LogVolId{i + 1}, glm::dmat4{1}}};
        }
        geometry.logVols.emplace(volume.id, std::move(volume));
    }
    return geometry;
}
} // namespace

TEST_CASE("Shared geometry validates repeated prototypes without expanding occurrences",
          "[ir][shared]") {
    auto geometry = repeatedGeometry();
    REQUIRE(geometry.validate() == 7);
    REQUIRE(geometry.logVols.size() == 3);
    REQUIRE(geometry.shapes.size() == 1);
    REQUIRE(geometry.materials.size() == 1);

    const auto tree = geometry.makeTree(1);
    REQUIRE(tree->cachedCount() == 0);
    REQUIRE(tree->materializationCount() == 0);
    REQUIRE(tree->materialize({0, 0})->originalPath == "/world/left/sensor");
    REQUIRE(tree->materialize({1, 0})->worldTransform[3][0] == Catch::Approx(-10));
}

TEST_CASE("Shared geometry counts a trillion physical placements with forty definitions",
          "[ir][shared]") {
    auto geometry = binaryLevels(40);
    REQUIRE(geometry.validate() == (uint64_t{1} << 40) - 1);
    REQUIRE(geometry.logVols.size() == 40);

    const auto tree = geometry.makeTree(0);
    REQUIRE(tree->materializationCount() == 0);
    REQUIRE(tree->cachedCount() == 0);
}

TEST_CASE("Shared geometry rejects dangling references and expanded nodes", "[ir][shared]") {
    auto geometry = repeatedGeometry();
    SECTION("missing root") { geometry.root.logVolId = LogVolId{99}; }
    SECTION("missing daughter volume") {
        geometry.logVols.at(LogVolId{2}).daughters.front().logVolId = LogVolId{99};
    }
    SECTION("missing shape") { geometry.logVols.at(LogVolId{3}).shapeId = ShapeId{99}; }
    SECTION("missing material") { geometry.logVols.at(LogVolId{3}).materialId = MaterialId{99}; }
    SECTION("boolean operand does not exist") {
        geometry.shapes.at(ShapeId{1}).data = BooleanUnion{ShapeId{2}, ShapeId{99}, glm::dmat4{1}};
        geometry.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{1, 1, 1}});
    }
    REQUIRE_THROWS(geometry.validate());
    REQUIRE_THROWS(geometry.makeTree());
}

TEST_CASE("Shared geometry rejects prototype cycles boolean cycles and count overflow",
          "[ir][shared]") {
    auto geometry = repeatedGeometry();
    SECTION("prototype self cycle") {
        geometry.logVols.at(LogVolId{2}).daughters.front().logVolId = LogVolId{2};
    }
    SECTION("prototype cycle through ancestors") {
        geometry.logVols.at(LogVolId{3}).daughters.push_back({"back", LogVolId{1}, glm::dmat4{1}});
    }
    SECTION("boolean self cycle") {
        geometry.shapes.at(ShapeId{1}).data = BooleanUnion{ShapeId{1}, ShapeId{1}, glm::dmat4{1}};
    }
    SECTION("indirect boolean cycle") {
        geometry.shapes.at(ShapeId{1}).data =
            BooleanSubtraction{ShapeId{2}, ShapeId{3}, glm::dmat4{1}};
        geometry.shapes.emplace(
            ShapeId{2},
            Shape{ShapeId{2}, BooleanIntersection{ShapeId{1}, ShapeId{3}, glm::dmat4{1}}});
        geometry.shapes.emplace(ShapeId{3}, Shape{ShapeId{3}, BoxShape{1, 1, 1}});
    }
    SECTION("physical count exceeds uint64 without expansion") { geometry = binaryLevels(65); }
    REQUIRE_THROWS(geometry.validate());
}

TEST_CASE("Shared geometry rejects invalid transform matrices", "[ir][shared]") {
    auto geometry = repeatedGeometry();
    SECTION("root NaN") {
        geometry.root.localTransform[3][0] = std::numeric_limits<double>::quiet_NaN();
    }
    SECTION("daughter infinity") {
        geometry.logVols.at(LogVolId{2}).daughters.front().localTransform[0][0] =
            std::numeric_limits<double>::infinity();
    }
    SECTION("projective root transform") { geometry.root.localTransform[0][3] = 0.5; }
    SECTION("invalid homogeneous coordinate") { geometry.root.localTransform[3][3] = 0; }
    SECTION("boolean operand NaN transform") {
        glm::dmat4 transform{1};
        transform[2][1] = std::numeric_limits<double>::quiet_NaN();
        geometry.shapes.at(ShapeId{1}).data = BooleanUnion{ShapeId{2}, ShapeId{2}, transform};
        geometry.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{1, 1, 1}});
    }
    REQUIRE_THROWS(geometry.validate());
}

TEST_CASE("Shared geometry validates metadata references before exposing a tree", "[ir][shared]") {
    auto geometry = repeatedGeometry();
    geometry.metadata.tagSets = {{{"sensitive", "true"}}};
    SECTION("volume metadata references missing volume") {
        geometry.metadata.volumeTags.emplace(LogVolId{99}, 0);
    }
    SECTION("placement metadata references missing parent") {
        geometry.metadata.placementTags.emplace(PlacementKey{LogVolId{99}, 0}, 0);
    }
    SECTION("placement metadata references missing daughter") {
        geometry.metadata.placementTags.emplace(PlacementKey{LogVolId{2}, 2}, 0);
    }
    SECTION("volume tag set ID is out of bounds") {
        geometry.metadata.volumeTags.emplace(LogVolId{3}, 1);
    }
    SECTION("placement tag set ID is out of bounds") {
        geometry.metadata.placementTags.emplace(PlacementKey{LogVolId{2}, 0}, 1);
    }
    SECTION("occurrence tag set ID is out of bounds") {
        geometry.metadata.occurrenceTags.emplace(OccurrenceId{0, 0}, 1);
    }
    SECTION("occurrence identity indexes a missing daughter") {
        geometry.metadata.occurrenceTags.emplace(OccurrenceId{0, 2}, 0);
    }
    SECTION("display name identity descends beyond leaf") {
        geometry.metadata.displayNames.emplace(OccurrenceId{0, 0, 0}, "invalid");
    }
    SECTION("path segment identity indexes a missing branch") {
        geometry.metadata.pathNames.emplace(OccurrenceId{2}, "invalid");
    }
    SECTION("provenance identity indexes a missing branch") {
        geometry.metadata.sourceSystems.emplace(OccurrenceId{2}, "invalid");
    }
    REQUIRE_THROWS(geometry.validate());
    REQUIRE_THROWS(geometry.makeTree());
}

TEST_CASE("Shared geometry tree preserves reusable metadata empty overrides and provenance",
          "[ir][shared]") {
    auto geometry = repeatedGeometry();
    geometry.metadata.tagSets = {
        {{"sensitive", "true"}, {"readout", "volume"}},
        {{"readout", "placement"}},
        {},
    };
    geometry.metadata.volumeTags.emplace(LogVolId{3}, 0);
    geometry.metadata.placementTags.emplace(PlacementKey{LogVolId{2}, 0}, 1);
    geometry.metadata.occurrenceTags.emplace(OccurrenceId{0, 0}, 2);
    geometry.metadata.displayNames.emplace(OccurrenceId{0, 0}, "PixelSensor");
    geometry.metadata.defaultSourceSystem = "dd4hep/tgeo";
    geometry.metadata.sourceSystems.emplace(OccurrenceId{0, 0}, "dd4hep/readout");
    geometry.metadata.sourceSystems.emplace(OccurrenceId{1, 1}, "");
    REQUIRE(geometry.validate() == 7);
    const auto tree = geometry.makeTree(1);
    const auto overridden = tree->materialize({0, 0});
    REQUIRE(overridden->name == "PixelSensor");
    REQUIRE(overridden->originalPath == "/world/left/sensor");
    REQUIRE(overridden->tags.empty());
    REQUIRE(overridden->sourceSystem == "dd4hep/readout");
    const auto repeated = tree->materialize({1, 0});
    REQUIRE(repeated->name == "sensor");
    REQUIRE(repeated->tags.at("readout") == "placement");
    REQUIRE(repeated->tags.at("sensitive") == "true");
    REQUIRE(repeated->sourceSystem == "dd4hep/tgeo");
    const auto sibling = tree->materialize({0, 1});
    REQUIRE(sibling->tags.at("readout") == "volume");
    REQUIRE(sibling->sourceSystem == "dd4hep/tgeo");
    REQUIRE(tree->materialize({1, 1})->sourceSystem.empty());
    REQUIRE(tree->materialize({})->sourceSystem == "dd4hep/tgeo");
    tree->clearCache();
    const auto rebuilt = tree->materialize({0, 0});
    REQUIRE(rebuilt->tags.empty());
    REQUIRE(rebuilt->name == overridden->name);
    REQUIRE(rebuilt->originalPath == overridden->originalPath);
    REQUIRE(rebuilt->sourceSystem == overridden->sourceSystem);
    REQUIRE(geometry.metadata.tagSets.size() == 3);
    REQUIRE(geometry.metadata.occurrenceTags.at(OccurrenceId{0, 0}) == 2);
}

TEST_CASE("Shared geometry accepts shared boolean operands and finite affine transforms",
          "[ir][shared]") {
    auto geometry = repeatedGeometry();
    const auto affine = glm::scale(glm::rotate(glm::translate(glm::dmat4{1}, glm::dvec3{3, 4, 5}),
                                               glm::radians(30.0), glm::dvec3{0, 0, 1}),
                                   glm::dvec3{-1, 2, 3});
    geometry.root.localTransform = affine;
    geometry.shapes.at(ShapeId{1}).data = BooleanUnion{ShapeId{2}, ShapeId{2}, affine};
    geometry.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, BoxShape{1, 2, 3}});
    REQUIRE(geometry.validate() == 7);
    const auto tree = geometry.makeTree(0);
    REQUIRE(tree->materialize({})->worldTransform == affine);
}

TEST_CASE("Shared imported path segments invalidate descendants but preserve pinned snapshots",
          "[ir][shared][occurrence]") {
    auto geometry = repeatedGeometry();
    geometry.metadata.pathNames.emplace(OccurrenceId{}, "detector");
    geometry.metadata.pathNames.emplace(OccurrenceId{0}, "module");
    auto tree = geometry.makeTree(4);
    const auto before = tree->materialize({0, 0});
    REQUIRE(before->originalPath == "/detector/module/sensor");
    tree->setPathName({0}, "corrected");
    REQUIRE(tree->materialize({0, 0})->originalPath == "/detector/corrected/sensor");
    REQUIRE(before->originalPath == "/detector/module/sensor");
    REQUIRE(tree->materialize({1, 0})->originalPath == "/detector/right/sensor");
    tree->clearCache();
    REQUIRE(tree->materialize({0, 0})->originalPath == "/detector/corrected/sensor");
}
