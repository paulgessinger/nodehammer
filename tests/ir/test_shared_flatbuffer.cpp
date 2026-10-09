#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cstring>
#include <detail/zstd_io.hpp>
#include <ir/legacy/nhs8.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <ir/semantic/occurrence_tree.hpp>
#include <ir/semantic_json.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <shared_generated.h>

using namespace nodehammer;
using namespace nodehammer::ir;
using namespace nodehammer::ir::semantic;
using nodehammer::ir::expanded::Node;
using nodehammer::ir::expanded::NodeId;
using namespace nodehammer::ir::semantic;
namespace {
semantic::Scene sample() {
    semantic::Scene geometry;
    auto &scene = geometry;
    scene.sourceFile = "/original/path/with space/geometry.xml";
    const MaterialId material{(1ULL << 54) + 7};
    scene.materials.emplace(material, SourceMaterial{material, "Silicon µ",
                                                     glm::vec3{0.1f, 0.2f, 0.3f}, 2.329123456789});
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{0.123456789012345, 2, 3}});
    scene.shapes.emplace(ShapeId{2}, Shape{ShapeId{2}, UnknownShape{"TGeoFutureSolid"}});
    glm::dmat4 transform{1.0};
    transform[3][0] = 1.23456789012345;
    scene.shapes.emplace(ShapeId{3},
                         Shape{ShapeId{3}, BooleanUnion{ShapeId{1}, ShapeId{2}, transform}});
    for (uint64_t id = 1; id <= 3; ++id) {
        scene.logVols.emplace(LogVolId{id},
                              LogicalVolume{LogVolId{id}, "volume", ShapeId{id}, material});
    }
    scene.logVols.at(LogVolId{1}).daughters = {{"module", LogVolId{2}, transform},
                                               {"module", LogVolId{2}, glm::dmat4{1}}};
    scene.logVols.at(LogVolId{2}).daughters = {{"fiber", LogVolId{3}, transform},
                                               {"fiber", LogVolId{3}, glm::dmat4{1}},
                                               {"third", LogVolId{3}, transform}};
    geometry.root = {"world", LogVolId{1}, transform};
    auto &metadata = geometry.metadata;
    metadata.tagSets = {{{"sensitive", "true"}}, {{"sensitive", "false"}}, {}};
    metadata.volumeTags.emplace(LogVolId{3}, 0);
    metadata.placementTags.emplace(PlacementKey{LogVolId{2}, 1}, 1);
    metadata.occurrenceTags.emplace(OccurrenceId{1, 1}, 2); // Explicitly clears defaults.
    metadata.displayNames.emplace(OccurrenceId{0, 2}, "renamed display only");
    metadata.pathNames.emplace(OccurrenceId{0}, "imported path segment");
    metadata.defaultSourceSystem = "dd4hep/tgeo";
    metadata.sourceSystems.emplace(OccurrenceId{1, 2}, "custom-source");
    return geometry;
}

// Modify a scalar at its vtable field offset to exercise verified but invalid files.
template <class T> void setField(void *object, flatbuffers::voffset_t field, T value) {
    auto *table = static_cast<uint8_t *>(object);
    const auto distance = flatbuffers::ReadScalar<flatbuffers::soffset_t>(table);
    auto *vtable = table - distance;
    auto offset = flatbuffers::ReadScalar<flatbuffers::voffset_t>(vtable + field);
    REQUIRE(offset != 0);
    flatbuffers::WriteScalar(table + offset, value);
}
} // namespace

TEST_CASE("NHS9 preserves shared identities and all sparse metadata",
          "[shared][serialization][flatbuffer]") {
    auto original = sample();
    original.metadata.originalPaths.emplace(OccurrenceId{0, 1}, "/old/path/retained");
    DegradationFlags flags;
    flags.set(DegradationBit::TransformApprox);
    original.metadata.degradation.emplace(OccurrenceId{1}, flags);
    original.root.localTransform[0][1] = -0.0;
    auto raw = sceneToBytes(original);
    REQUIRE(std::memcmp(raw.data() + 4, "NHS9", 4) == 0);
    REQUIRE(raw == sceneToBytes(original));
    for (bool compressed : {false, true}) {
        auto data = compressed ? detail::zstd_io::compress(raw) : raw;
        auto restored = sceneFromBytes(data);
        REQUIRE(restored.validate() == 9);

        REQUIRE(restored.materials.begin()->first.value == (1ULL << 54) + 7);
        REQUIRE(restored.sourceFile == original.sourceFile);
        REQUIRE(std::get<UnknownShape>(restored.shapes.at(ShapeId{2}).data).originalType ==
                "TGeoFutureSolid");
        REQUIRE(std::get<BooleanUnion>(restored.shapes.at(ShapeId{3}).data).rightTransform ==
                std::get<BooleanUnion>(original.shapes.at(ShapeId{3}).data).rightTransform);
        REQUIRE(std::bit_cast<uint64_t>(restored.root.localTransform[0][1]) ==
                std::bit_cast<uint64_t>(-0.0));
        REQUIRE(restored.metadata.tagSets == original.metadata.tagSets);
        REQUIRE(restored.metadata.volumeTags == original.metadata.volumeTags);
        REQUIRE(restored.metadata.placementTags == original.metadata.placementTags);
        REQUIRE(restored.metadata.occurrenceTags == original.metadata.occurrenceTags);
        REQUIRE(restored.metadata.displayNames == original.metadata.displayNames);
        REQUIRE(restored.metadata.pathNames == original.metadata.pathNames);
        REQUIRE(restored.metadata.originalPaths == original.metadata.originalPaths);
        REQUIRE(restored.metadata.degradation.at({1}).bits == flags.bits);
        REQUIRE(restored.metadata.sourceSystems == original.metadata.sourceSystems);
        REQUIRE(restored.metadata.defaultSourceSystem == original.metadata.defaultSourceSystem);
        REQUIRE(sceneToBytes(restored) == raw);
    }
}
TEST_CASE("NHS9 rejects unsupported versions and malformed references",
          "[shared][serialization][flatbuffer]") {
    const auto original = sceneToBytes(sample());
    namespace fb = nodehammer::shared_fbs;
    SECTION("version") {
        auto bytes = original;
        auto *root = const_cast<fb::SharedGeometry *>(fb::GetSharedGeometry(bytes.data()));
        setField(root, fb::SharedGeometry::VT_SCHEMA_VERSION, uint32_t{99});
        REQUIRE_THROWS_WITH(sceneFromBytes(bytes),
                            "NHS9: unsupported schema version or required features");
    }
    SECTION("required feature") {
        auto bytes = original;
        auto *root = const_cast<fb::SharedGeometry *>(fb::GetSharedGeometry(bytes.data()));
        setField(root, fb::SharedGeometry::VT_REQUIRED_FEATURES, uint64_t{1});
        REQUIRE_THROWS_WITH(sceneFromBytes(bytes),
                            "NHS9: unsupported schema version or required features");
    }
    SECTION("transform") {
        auto bytes = original;
        auto *root = const_cast<fb::Placement *>(fb::GetSharedGeometry(bytes.data())->root());
        setField(root, fb::Placement::VT_TRANSFORM_INDEX, std::numeric_limits<uint32_t>::max());
        REQUIRE_THROWS(sceneFromBytes(bytes));
    }
    SECTION("volume") {
        auto bytes = original;
        auto *root = const_cast<fb::Placement *>(fb::GetSharedGeometry(bytes.data())->root());
        setField(root, fb::Placement::VT_LOG_VOL_ID, uint64_t{987654321});
        REQUIRE_THROWS(sceneFromBytes(bytes));
    }
    SECTION("declared count") {
        auto bytes = original;
        auto *root = const_cast<fb::SharedGeometry *>(fb::GetSharedGeometry(bytes.data()));
        setField(root, fb::SharedGeometry::VT_OCCURRENCE_COUNT, uint64_t{8});
        REQUIRE_THROWS_WITH(sceneFromBytes(bytes), "NHS9: occurrence count mismatch");
    }
    SECTION("truncated") {
        for (std::size_t length : {std::size_t{0}, std::size_t{7}, original.size() / 2})
            REQUIRE_THROWS(sceneFromBytes(std::span{original}.first(length)));
    }
    SECTION("compressed trailing bytes") {
        auto bytes = detail::zstd_io::compress(original);
        bytes.push_back(std::byte{0});
        REQUIRE_THROWS(sceneFromBytes(bytes));
    }
}
TEST_CASE("NHS9 stores billion-occurrence geometry without expanded nodes",
          "[shared][serialization][flatbuffer]") {
    auto geometry = sample();
    geometry.metadata = {};
    geometry.logVols.clear();
    for (uint64_t id = 1; id <= 10; ++id) {
        LogicalVolume v{LogVolId{id}, "repeated", ShapeId{1}, geometry.materials.begin()->first};
        if (id < 10)
            for (int i = 0; i < 10; ++i)
                v.daughters.push_back({"same", LogVolId{id + 1}, glm::dmat4{1}});
        geometry.logVols.emplace(v.id, std::move(v));
    }
    auto bytes = sceneToBytes(geometry);
    REQUIRE(bytes.size() < 20'000);
    auto restored = sceneFromBytes(bytes);
    REQUIRE(restored.validate() == 1'111'111'111);
}
TEST_CASE("Shared byte reader adapts NHS8 stored nodes", "[shared][serialization][flatbuffer]") {
    expanded::Scene scene;
    scene.materials.emplace(MaterialId{1}, SourceMaterial{MaterialId{1}, "air", {}, 1});
    scene.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{1, 2, 3}});
    scene.logVols.emplace(LogVolId{1},
                          LogicalVolume{LogVolId{1}, "volume", ShapeId{1}, MaterialId{1}});
    Node node;
    node.id = NodeId{1};
    node.name = "stored node";
    node.logVolId = LogVolId{1};
    node.originalPath = "/stored";
    scene.rootId = node.id;
    scene.nodes.emplace(node.id, node);
    auto bytes = nodehammer::ir::legacy::nhs8::semanticSceneToBytes(scene);
    for (bool compressed : {false, true}) {
        auto data = compressed ? detail::zstd_io::compress(bytes) : bytes;
        auto upgraded = sceneFromBytes(data);
        REQUIRE(upgraded.validate() == 1);

        REQUIRE(upgraded.makeTree()->materialize({})->name == "stored node");
        REQUIRE(upgraded.metadata.originalPaths.at({}) == "/stored node");
    }
}

TEST_CASE("NHS9 covers every typed shape payload", "[shared][serialization][flatbuffer]") {
    auto geometry = sample();
    glm::dmat4 booleanTransform{1};
    booleanTransform[0][0] = 1.25;
    booleanTransform[1][0] = 0.125;
    booleanTransform[3][2] = 123.456789012345;
    const std::vector<ShapeVariant> variants{
        TubeShape{1, 2, 3, 0.2, 1.1},
        ConeShape{1, 2, 3, 4, 5, 0.3, 1.2},
        TrdShape{1, 2, 3, 4, 5},
        ParaShape{1, 2, 3, 0.1, 0.2, 0.3},
        PconShape{0.1, 1.2, {{-2, 1, 3}, {2, 1, 4}}},
        PgonShape{0.2, 1.3, 7, {{-2, 1, 3}, {2, 1, 4}}},
        TorusShape{1, 2, 3, 0.1, 1.4},
        TessellatedShape{{{{glm::dvec3{1, 2, 3}, glm::dvec3{2, 3, 4}, glm::dvec3{3, 4, 5}}}}},
        BooleanIntersection{ShapeId{1}, ShapeId{2}, booleanTransform},
        BooleanSubtraction{ShapeId{1}, ShapeId{2}, booleanTransform}};
    uint64_t id = (1ULL << 40);
    for (const auto &variant : variants) {
        ShapeId key{++id};
        geometry.shapes.emplace(key, Shape{key, variant});
    }
    auto bytes = sceneToBytes(geometry);
    auto restored = sceneFromBytes(bytes);
    REQUIRE(restored.shapes.size() == geometry.shapes.size());
    for (const auto &[key, shape] : geometry.shapes) {
        const auto &copy = restored.shapes.at(key);
        REQUIRE(nlohmann::json(copy) == nlohmann::json(shape));
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (is_boolean_shape_v<T>) {
                    REQUIRE(std::get<T>(copy.data).rightTransform == value.rightTransform);
                }
            },
            shape.data);
    }
    REQUIRE(sceneToBytes(restored) == bytes);
}
