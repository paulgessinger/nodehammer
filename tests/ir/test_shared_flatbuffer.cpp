#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

TEST_CASE("NHS9 compact columns preserve catalog IDs and affine matrix bits",
          "[shared][serialization][flatbuffer]") {
    const uint64_t id =
        GENERATE(uint64_t{0}, uint64_t{65536}, std::numeric_limits<uint64_t>::max());
    Scene original;
    original.materials.emplace(MaterialId{id},
                               SourceMaterial{MaterialId{id}, "material", std::nullopt, 0.0});
    original.shapes.emplace(ShapeId{id}, Shape{ShapeId{id}, BoxShape{1, 2, 3}});
    original.logVols.emplace(LogVolId{id},
                             LogicalVolume{LogVolId{id}, "volume", ShapeId{id}, MaterialId{id}});
    original.root = {"root", LogVolId{id}, glm::dmat4{1}};
    original.root.localTransform[0][3] = -0.0;
    original.root.localTransform[0][1] = 0.125;
    original.root.localTransform[1][1] = 1.125;
    original.metadata.tagSets = {{}};
    original.metadata.occurrenceTags.emplace(OccurrenceId{}, 0);
    original.metadata.displayNames.emplace(OccurrenceId{}, "display");
    original.metadata.pathNames.emplace(OccurrenceId{}, "path");
    const auto bytes = sceneToBytes(original);
    const auto *fb = nodehammer::shared_fbs::GetSharedGeometry(bytes.data());
    REQUIRE(fb->schema_version() == 6);
    REQUIRE(fb->metadata()->path_offset_deltas()->size() == 2);
    const auto copy = sceneFromBytes(bytes);
    REQUIRE(copy.materials.contains(MaterialId{id}));
    REQUIRE(copy.shapes.contains(ShapeId{id}));
    REQUIRE(copy.logVols.contains(LogVolId{id}));
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            REQUIRE(std::bit_cast<uint64_t>(copy.root.localTransform[col][row]) ==
                    std::bit_cast<uint64_t>(original.root.localTransform[col][row]));
    REQUIRE(copy.metadata.occurrenceTags == original.metadata.occurrenceTags);
    REQUIRE(sceneToBytes(copy) == bytes);
}

namespace {
template <class T> void setColumn(const flatbuffers::Vector<T> *column, uint32_t row, T value) {
    flatbuffers::WriteScalar(const_cast<uint8_t *>(column->Data()) + sizeof(T) * row, value);
}
} // namespace
TEST_CASE("NHS9 compact columns reject malformed topology and metadata",
          "[shared][serialization][flatbuffer]") {
    auto original = sample();
    DegradationFlags flags;
    flags.set(DegradationBit::TransformApprox);
    original.metadata.degradation.emplace(OccurrenceId{0}, flags);
    auto bytes = sceneToBytes(original);
    const auto *root = nodehammer::shared_fbs::GetSharedGeometry(bytes.data());
    const auto *v = root->volumes();
    const auto *m = root->metadata();
    SECTION("mismatched lengths") {
        flatbuffers::WriteScalar(const_cast<flatbuffers::Vector<uint32_t> *>(v->shapes()),
                                 uint32_t{1});
    }
    SECTION("first offset") { setColumn(v->daughter_offset_deltas(), 0, uint32_t{1}); }
    SECTION("last offset") { setColumn(v->daughter_offset_deltas(), 3, uint32_t{1}); }
    SECTION("unordered offset") { setColumn(v->daughter_offset_deltas(), 1, uint32_t{99}); }
    SECTION("invalid volume") { setColumn(v->daughter_volume_deltas(), 0, uint32_t{99}); }
    SECTION("zero reference") { setColumn(v->daughter_volume_deltas(), 0, uint32_t{0}); }
    SECTION("invalid shape") { setColumn(v->shapes(), 0, uint32_t{99}); }
    SECTION("invalid material") { setColumn(v->materials(), 0, uint32_t{99}); }
    SECTION("invalid transform") { setColumn(v->daughter_transforms(), 0, uint32_t{999}); }
    SECTION("duplicate ID") { setColumn(v->id_deltas(), 1, uint64_t{0}); }
    SECTION("invalid path offset") { setColumn(m->path_offset_deltas(), 0, uint32_t{1}); }
    SECTION("invalid path reference") {
        setColumn(m->occurrence_tags()->key_deltas(), 0, uint32_t{999});
    }
    SECTION("invalid path component") { setColumn(m->path_components(), 0, uint64_t{999}); }
    SECTION("path component exceeds platform or topology bounds") {
        setColumn(m->path_components(), 0, std::numeric_limits<uint64_t>::max());
    }
    SECTION("invalid tag set") { setColumn(m->occurrence_tags()->values(), 0, uint32_t{999}); }
    SECTION("invalid placement daughter") { setColumn(m->placement_daughters(), 0, uint64_t{999}); }
    SECTION("invalid metadata volume") {
        setColumn(m->volume_tags()->key_deltas(), 0, uint32_t{999});
    }
    SECTION("invalid flags") {
        setColumn(m->degradation_flags(), 0, std::numeric_limits<uint64_t>::max());
    }
    SECTION("mismatched metadata lengths") {
        flatbuffers::WriteScalar(
            const_cast<flatbuffers::Vector<uint32_t> *>(m->occurrence_tags()->values()),
            uint32_t{0});
    }
    SECTION("invalid daughter name dictionary reference") {
        setColumn(v->daughter_name_indices(), 0, root->strings()->prefix_lengths()->size());
    }
    SECTION("older development revisions") {
        setField(const_cast<nodehammer::shared_fbs::SharedGeometry *>(root),
                 nodehammer::shared_fbs::SharedGeometry::VT_SCHEMA_VERSION,
                 GENERATE(uint32_t{1}, uint32_t{2}, uint32_t{3}, uint32_t{4}, uint32_t{5}));
    }

    SECTION("invalid volume name dictionary reference") {
        setColumn(v->name_index_deltas(), 0, std::numeric_limits<uint32_t>::max());
    }
    SECTION("invalid override dictionary reference") {
        setColumn(m->display_names()->value_index_deltas(), 0,
                  std::numeric_limits<uint32_t>::max());
    }
    SECTION("invalid string prefix") {
        setColumn(root->strings()->prefix_lengths(), 0, uint32_t{1});
    }
    SECTION("invalid string suffix") {
        setColumn(root->strings()->suffix_lengths(), 0, std::numeric_limits<uint32_t>::max());
    }
    SECTION("mismatched string column lengths") {
        flatbuffers::WriteScalar(
            const_cast<flatbuffers::Vector<uint32_t> *>(root->strings()->suffix_lengths()),
            uint32_t{0});
    }
    SECTION("trailing string suffix bytes") {
        const auto *lengths = root->strings()->suffix_lengths();
        REQUIRE(lengths->Get(lengths->size() - 1) > 0);
        setColumn(lengths, lengths->size() - 1, lengths->Get(lengths->size() - 1) - 1);
    }
    SECTION("mismatched path prefix lengths") {
        flatbuffers::WriteScalar(
            const_cast<flatbuffers::Vector<uint32_t> *>(m->path_prefix_lengths()), uint32_t{0});
    }
    SECTION("invalid path prefix") { setColumn(m->path_prefix_lengths(), 0, uint32_t{1}); }
    SECTION("offset deltas overflow reconstructed bounds") {
        setColumn(v->daughter_offset_deltas(), 1, std::numeric_limits<uint32_t>::max());
        setColumn(v->daughter_offset_deltas(), 2, uint32_t{1});
    }

    SECTION("missing required column table") {
        setField(const_cast<nodehammer::shared_fbs::SharedGeometry *>(root),
                 nodehammer::shared_fbs::SharedGeometry::VT_VOLUMES, uint32_t{0});
    }
    REQUIRE_THROWS(sceneFromBytes(bytes));
}

TEST_CASE("NHB decoding reseeds monotonic catalog allocators",
          "[shared][serialization][flatbuffer]") {
    auto restored = sceneFromBytes(sceneToBytes(sample()));
    REQUIRE(restored.nextLogVolId().value == 4);
    REQUIRE(restored.nextShapeId().value == 4);
    REQUIRE(restored.nextMaterialId().value == (1ULL << 54) + 8);
    restored.reseedIdCounters();
    REQUIRE(restored.nextLogVolId().value == 5);
}
TEST_CASE("Catalog ID exhaustion remains explicit after decoding",
          "[shared][serialization][flatbuffer]") {
    const auto maximum = std::numeric_limits<uint64_t>::max();
    auto original = sample();
    original.materials.emplace(MaterialId{maximum},
                               SourceMaterial{MaterialId{maximum}, "last", std::nullopt, 0});
    original.shapes.emplace(ShapeId{maximum}, Shape{ShapeId{maximum}, BoxShape{1, 1, 1}});
    original.logVols.emplace(
        LogVolId{maximum},
        LogicalVolume{LogVolId{maximum}, "last", ShapeId{maximum}, MaterialId{maximum}});
    auto restored = sceneFromBytes(sceneToBytes(original));
    REQUIRE(restored.materials.contains(MaterialId{maximum}));
    REQUIRE_THROWS_AS(restored.nextMaterialId(), std::overflow_error);
    REQUIRE_THROWS_AS(restored.nextShapeId(), std::overflow_error);
    REQUIRE_THROWS_AS(restored.nextLogVolId(), std::overflow_error);
    restored.reseedIdCounters();
    REQUIRE_THROWS_AS(restored.nextMaterialId(), std::overflow_error);
    GeometryCatalogs catalog;
    catalog.shapes.emplace(ShapeId{maximum - 1}, Shape{ShapeId{maximum - 1}, BoxShape{1, 1, 1}});
    catalog.reseedIdCounters();
    REQUIRE(catalog.nextShapeId().value == maximum);
    REQUIRE_THROWS_AS(catalog.nextShapeId(), std::overflow_error);
    catalog.reseedIdCounters();
    REQUIRE_THROWS_AS(catalog.nextShapeId(), std::overflow_error);
}

TEST_CASE("NHS9 delta columns preserve descending string references",
          "[shared][serialization][flatbuffer]") {
    auto original = sample();
    original.logVols.at(LogVolId{2}).name = "another volume";
    const auto bytes = sceneToBytes(original);
    const auto *columns = nodehammer::shared_fbs::GetSharedGeometry(bytes.data())->volumes();
    REQUIRE(columns->name_index_deltas()->Get(2) > std::numeric_limits<uint32_t>::max() / 2);
    auto restored = sceneFromBytes(bytes);
    for (const auto &[id, v] : original.logVols)
        REQUIRE(restored.logVols.at(id).name == v.name);
    REQUIRE(sceneToBytes(restored) == bytes);
}

TEST_CASE("NHS9 dictionary columns preserve empty and embedded-null strings",
          "[shared][serialization][flatbuffer]") {
    auto original = sample();
    original.logVols.at(LogVolId{1}).name = "";
    original.logVols.at(LogVolId{2}).name = std::string{"volume\0tail", 11};
    original.metadata.displayNames[{0, 2}] = std::string{"name\0tail", 9};
    original.metadata.sourceSystems[{1, 2}] = "";
    const auto bytes = sceneToBytes(original);
    auto copy = sceneFromBytes(bytes);
    REQUIRE(copy.logVols.at(LogVolId{1}).name.empty());
    REQUIRE(copy.logVols.at(LogVolId{2}).name == original.logVols.at(LogVolId{2}).name);
    REQUIRE(copy.metadata.displayNames == original.metadata.displayNames);
    REQUIRE(copy.metadata.sourceSystems == original.metadata.sourceSystems);
    REQUIRE(sceneToBytes(copy) == bytes);
}

TEST_CASE("NHS9 daughter volume deltas preserve repeats and descending references",
          "[shared][serialization][flatbuffer]") {
    auto original = sample();
    auto &daughters = original.logVols.at(LogVolId{1}).daughters;
    daughters = {{"leaf", LogVolId{3}, glm::dmat4{1}},
                 {"module", LogVolId{2}, glm::dmat4{1}},
                 {"module again", LogVolId{2}, glm::dmat4{1}}};
    original.metadata = {};
    const auto bytes = sceneToBytes(original);
    const auto *columns = nodehammer::shared_fbs::GetSharedGeometry(bytes.data())->volumes();
    REQUIRE(columns->daughter_volume_deltas()->Get(1) == std::numeric_limits<uint32_t>::max());
    REQUIRE(columns->daughter_volume_deltas()->Get(2) == 0);
    const auto restored = sceneFromBytes(bytes);
    REQUIRE(restored.logVols.at(LogVolId{1}).daughters.at(0).logVolId == LogVolId{3});
    REQUIRE(restored.logVols.at(LogVolId{1}).daughters.at(1).logVolId == LogVolId{2});
    REQUIRE(restored.logVols.at(LogVolId{1}).daughters.at(2).logVolId == LogVolId{2});
    REQUIRE(sceneToBytes(restored) == bytes);
}

TEST_CASE("NHS9 shares string and path prefixes without merging identities",
          "[shared][serialization][flatbuffer]") {
    auto original = sample();
    std::string prefix(1024, 'x');
    prefix[512] = '\0';
    original.logVols.at(LogVolId{1}).name = prefix + "alpha";
    original.logVols.at(LogVolId{2}).name = prefix + "beta";
    original.logVols.at(LogVolId{3}).name = prefix; // Shorter than its predecessor.
    for (auto &[id, volume] : original.logVols)
        for (auto &daughter : volume.daughters)
            daughter.name = prefix + "alpha";
    original.metadata.originalPaths[{}] = "/world";
    original.metadata.originalPaths[{0, 0}] = "/world/common/first";
    original.metadata.originalPaths[{0, 1}] = "/world/common/second";
    original.metadata.originalPaths[{0, 2}] = "/world/common/third";
    const auto bytes = sceneToBytes(original);
    const auto *root = nodehammer::shared_fbs::GetSharedGeometry(bytes.data());
    REQUIRE(root->strings()->suffix_bytes()->size() < 2 * prefix.size());
    bool sharedPath = false;
    for (const auto count : *root->metadata()->path_prefix_lengths())
        sharedPath |= count > 0;
    REQUIRE(sharedPath);
    const auto copy = sceneFromBytes(bytes);
    REQUIRE(copy.logVols.size() == original.logVols.size());
    for (const auto &[id, volume] : original.logVols) {
        REQUIRE(copy.logVols.at(id).name == volume.name);
        REQUIRE(copy.logVols.at(id).daughters.size() == volume.daughters.size());
    }
    REQUIRE(copy.metadata.originalPaths == original.metadata.originalPaths);
    REQUIRE(copy.metadata.placementTags == original.metadata.placementTags);
    REQUIRE(copy.metadata.occurrenceTags == original.metadata.occurrenceTags);
    REQUIRE(sceneToBytes(copy) == bytes);
}
