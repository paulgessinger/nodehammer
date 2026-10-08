#include "cli_test_support.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <fstream>
#include <ir/expanded/conversion.hpp>
#include <ir/fb/render/flatbuffer.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/fb/semantic/importer.hpp>
#include <ir/legacy/nhs8.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <nodehammer/nhb.hpp>
#include <scene_build.hpp>
#include <selection/occurrence_selector.hpp>
#include <selection/selector.hpp>
#include <tessellation/build_pipeline.hpp>

namespace {
struct Files {
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("nh-shared-cli-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Files() { std::filesystem::create_directories(dir); }
    ~Files() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::string path(const char *name) const { return (dir / name).string(); }
};
nodehammer::ir::semantic::Scene repeated(unsigned levels = 3) {
    using namespace nodehammer::ir::semantic;
    using nodehammer::ir::expanded::Node;
    using nodehammer::ir::expanded::NodeId;
    nodehammer::ir::semantic::Scene g;
    g.root = {"world", LogVolId{1}, glm::dmat4{1}};
    g.shapes.emplace(ShapeId{1}, Shape{ShapeId{1}, BoxShape{1, 1, 1}});
    g.materials.emplace(MaterialId{1},
                        SourceMaterial{MaterialId{1}, "silicon", std::nullopt, 2.33});
    for (uint64_t i = 1; i <= levels; ++i) {
        LogicalVolume v{LogVolId{i}, "volume", ShapeId{1}, MaterialId{1}};
        if (i < levels) {
            auto transform = glm::dmat4{1};
            transform[3][0] = 10;
            v.daughters = {{"left", LogVolId{i + 1}, glm::dmat4{1}},
                           {"right", LogVolId{i + 1}, transform}};
        }
        g.logVols.emplace(v.id, std::move(v));
    }
    g.metadata.tagSets = {{{"sensitive", "true"}}};
    g.metadata.volumeTags.emplace(LogVolId{levels}, 0);
    return g;
}
} // namespace
TEST_CASE("normal commands read shared NHS9 and select before rendering", "[cli][shared]") {
    Files files;
    const auto input = files.path("input.nhb.zst");
    nodehammer::ir::semantic::writeFlatbuffer(repeated(), input);
    auto imported = nodehammer::ir::FlatBufferImporter{}.import(input);
    REQUIRE(imported.scene.logVols.size() == 3);
    REQUIRE(imported.scene.nodeCount() == 7);
    auto summary =
        nhtest::runCaptured({"inspect", "--output-format", "json", "summary", "-i", input});
    REQUIRE(summary.code == 0);
    REQUIRE(nlohmann::json::parse(summary.out).at("nodes") == 7);
    auto tags = nhtest::runCaptured({"inspect", "--output-format", "json", "tags", "-i", input});
    REQUIRE(tags.code == 0);
    REQUIRE(nlohmann::json::parse(tags.out).at("nodesWithTags") == 4);
    auto tree = nhtest::runCaptured(
        {"inspect", "--output-format", "json", "tree", "-i", input, "--depth", "1"});
    REQUIRE(tree.code == 0);
    REQUIRE(nlohmann::json::parse(tree.out).at("shown") == 3);
    const auto config = files.path("selection.toml");
    {
        std::ofstream out(config);
        out << "[[selection_rules]]\ndrop_if = 'path ~= \"/world/right/**\" || path == "
               "\"/world/right\"'\n";
    }
    const auto semantic = files.path("selected.nhb.zst");
    const auto render = files.path("selected.nhr");
    auto result =
        nhtest::runCaptured({"convert", "-i", input, "-c", config, "-o", semantic, "-o", render});
    INFO(result.err);
    REQUIRE(result.code == 0);
    const auto selected = nodehammer::ir::semantic::readFlatbuffer(semantic);
    REQUIRE(selected.validate() == 4);

    const auto mesh = nodehammer::ir::renderSceneFromBytes(
        nodehammer::detail::zstd_io::readBytesFromFile(render));
    REQUIRE(mesh.nodes.size() == 4);
    REQUIRE_FALSE(mesh.meshAssets.empty());
    auto reloaded = nodehammer::ir::FlatBufferImporter{}.import(semantic);
    const auto physical = nodehammer::ir::semantic::expand(reloaded.scene);
    REQUIRE(physical.nodes.size() == 4);
    for (const auto &[id, node] : physical.nodes) {
        (void)id;
        REQUIRE_FALSE(node.originalPath.starts_with("/world/right"));
    }
}
TEST_CASE("normal inspect and convert keep billion-occurrence NHS9 compact", "[cli][shared]") {
    Files files;
    const auto input = files.path("huge.nhb");
    const auto output = files.path("copy.nhb.zst");
    nodehammer::ir::semantic::writeFlatbuffer(repeated(30), input);
    auto summary =
        nhtest::runCaptured({"inspect", "--output-format", "json", "summary", "-i", input});
    REQUIRE(summary.code == 0);
    REQUIRE(nlohmann::json::parse(summary.out).at("nodes") == (uint64_t{1} << 30) - 1);
    auto tree = nhtest::runCaptured(
        {"inspect", "--output-format", "json", "tree", "-i", input, "--depth", "1"});
    REQUIRE(tree.code == 0);
    REQUIRE(nlohmann::json::parse(tree.out).at("shown") == 3);
    auto converted = nhtest::runCaptured({"convert", "-i", input, "-o", output});
    INFO(converted.err);
    REQUIRE(converted.code == 0);
    REQUIRE(nodehammer::ir::semantic::readFlatbuffer(output).validate() == (uint64_t{1} << 30) - 1);
    REQUIRE(std::filesystem::file_size(output) < 16384);
}
TEST_CASE("upgrade runs through the common CLI API and protects its input", "[cli][shared]") {
    Files files;
    const auto input =
        (std::filesystem::path{NODEHAMMER_FIXTURES_DIR} / "nhb/legacy-nhs8-mdi.nhb.zst").string();
    auto refused = nhtest::runCaptured({"upgrade", "-i", input, "-o", input});
    REQUIRE(refused.code != 0);
    REQUIRE(refused.err.find("must differ") != std::string::npos);
    const auto output = files.path("upgraded.nhb.zst");
    auto result = nhtest::runCaptured({"upgrade", "-i", input, "-o", output});
    INFO(result.err);
    REQUIRE(result.code == 0);
    // Capture reads raw bytes; Windows text-mode stdout emits CRLF.
    REQUIRE((result.out == output + "\n" || result.out == output + "\r\n"));
    REQUIRE(nodehammer::ir::semantic::readFlatbuffer(output).validate() == 99);
}
TEST_CASE("pipeline selection preserves expanded semantics and immutable shared input",
          "[shared][selection]") {
    using namespace nodehammer;
    auto geometry = repeated();
    geometry.metadata.originalPaths.emplace(ir::semantic::OccurrenceId{1, 0},
                                            "/world/old/right/left");
    const auto source = geometry;
    auto compact = source;
    auto expanded = ir::semantic::expand(source);
    config::SelectionRule rule;
    rule.action = config::SelectionAction::DropIf;
    rule.predicate = config::PredicateExpr{config::PathGlobPredicate{"/world/right"}};
    bool hoist = false;
    SECTION("drop subtree") {}
    SECTION("hoist descendants") { hoist = true; }
    auto tree = geometry.makeTree(0);
    const selection::SelectedOccurrences all{*tree};
    REQUIRE(all.subtreeSize({}) == 7);
    REQUIRE(all.subtreeSize({1}) == 3);
    const auto selected = all.prune({rule}, hoist);
    REQUIRE(selected.subtreeSize({1}) == (hoist ? 2 : 0));
    REQUIRE(selected.subtreeSize({}) == selected.size());
    const selection::SelectionEngine engine{{rule}, hoist};
    auto diags = engine.prune(compact);
    auto referenceDiags = engine.prune(expanded);
    REQUIRE(diags.hasErrors() == referenceDiags.hasErrors());
    const auto physical = ir::semantic::expand(compact);
    REQUIRE(physical.nodes.size() == expanded.nodes.size());
    REQUIRE(source.nodeCount() == 7);
    for (const auto &[id, expected] : expanded.nodes) {
        (void)id;
        const auto found =
            std::find_if(physical.nodes.begin(), physical.nodes.end(), [&](const auto &row) {
                return row.second.originalPath == expected.originalPath;
            });
        REQUIRE(found != physical.nodes.end());
        const auto &actual = found->second;
        REQUIRE(actual.localTransform == expected.localTransform);
        REQUIRE(actual.worldTransform == expected.worldTransform);
        REQUIRE(actual.tags == expected.tags);
        REQUIRE(actual.children.size() == expected.children.size());
        if (expected.parentId) {
            REQUIRE(actual.parentId);
            REQUIRE(physical.nodes.at(*actual.parentId).originalPath ==
                    expanded.nodes.at(*expected.parentId).originalPath);
        }
    }
}
TEST_CASE("viewer build pipeline consumes shared bytes without mutating its source",
          "[shared][build_pipeline]") {
    using namespace nodehammer;
    const auto bytes = ir::semantic::sceneToBytes(repeated());
    auto imported = ir::FlatBufferImporter::importFromBytes("scene.nhb", bytes);
    auto source = std::make_shared<const ir::semantic::Scene>(std::move(imported.scene));
    auto config = std::make_shared<config::NHConfig>();
    std::size_t expected = 7;
    SECTION("no selection") {}
    SECTION("filter before tessellation") {
        config::SelectionRule rule;
        rule.action = config::SelectionAction::DropIf;
        rule.predicate = config::PredicateExpr{config::PathGlobPredicate{"/world/right"}};
        config->selection.push_back(rule);
        expected = 4;
    }
    tessellation::BuildPipeline pipeline;
    pipeline.start(config, source, std::nullopt);
    while (!pipeline.advance(std::numeric_limits<uint64_t>::max())) {
    }
    auto result = pipeline.take();
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.scene);
    REQUIRE(result.scene->nodes.size() == expected);
    REQUIRE_FALSE(result.scene->meshAssets.empty());
    REQUIRE(source->nodeCount() == 7);
}
TEST_CASE("NHS8 reads warn through file byte and CLI paths but remain supported",
          "[cli][shared][legacy]") {
    using namespace nodehammer;
    Files files;
    const auto fixture =
        std::filesystem::path{NODEHAMMER_FIXTURES_DIR} / "nhb/legacy-nhs8-mdi.nhb.zst";
    const auto raw = detail::zstd_io::readBytesFromFile(fixture);
    for (bool compressed : {false, true}) {
        const auto bytes = compressed ? detail::zstd_io::compress(raw) : raw;
        const auto input = files.path(compressed ? "legacy.nhb.zst" : "legacy.nhb");
        detail::zstd_io::writeBytesToFile(input, raw);
        const auto file = ir::FlatBufferImporter{}.import(input);
        const auto memory = ir::FlatBufferImporter::importFromBytes("embedded-geometry", bytes);
        const auto api = fromNhb(bytes);
        for (const auto *diags : {&file.diags, &memory.diags, &api.diags}) {
            REQUIRE_FALSE(diags->hasErrors());
            REQUIRE(diags->size() == 1);
            const auto &warning = diags->items().front();
            CHECK(warning.code == codes::kWarnImportLegacyNhb);
            CHECK(warning.severity == Diagnostic::Severity::Warning);
            CHECK(warning.message.find("nodehammer upgrade") != std::string::npos);
            CHECK(warning.message.find("embedded geometry") != std::string::npos);
        }
        REQUIRE(api.scene.nodeCount() == 99);
        auto inspected =
            nhtest::runCaptured({"inspect", "--output-format", "json", "summary", "-i", input});
        REQUIRE(inspected.code == 0);
        const auto summary = nlohmann::json::parse(inspected.out);
        CHECK(summary.at("nodes") == 99);
        CHECK(summary.at("diagnostics").at("warnings") == 1);
        CHECK(inspected.err.find("NH0107") != std::string::npos);
        const auto strictOutput = files.path("strict.nhb.zst");
        auto strict = nhtest::runCaptured({"convert", "--strict", "-i", input, "-o", strictOutput});
        CHECK(strict.code != 0);
        CHECK(strict.err.find("NH0107") != std::string::npos);
        CHECK_FALSE(std::filesystem::exists(strictOutput));
        const auto output = files.path("upgraded.nhb.zst");
        auto upgraded = nhtest::runCaptured({"upgrade", "-i", input, "-o", output});
        REQUIRE(upgraded.code == 0);
        CHECK(upgraded.err.find("NH0107") == std::string::npos);
        CHECK(ir::FlatBufferImporter{}.import(output).diags.empty());
        CHECK(fromNhb(toNhb(api.scene)).diags.empty());
    }
}

TEST_CASE("normal processing transport always writes NHS9", "[cli][shared][serialization]") {
    using namespace nodehammer::ir;
    const auto scene = repeated();
    const auto bytes = semanticSceneToBytes(scene);
    REQUIRE(bytes.size() >= 8);
    CHECK(std::string_view(reinterpret_cast<const char *>(bytes.data() + 4), 4) == "NHS9");
    auto restored = semanticSceneFromBytes(bytes);
    REQUIRE(restored.nodeCount() == 7);
    const auto physical = semantic::expand(restored);
    for (const auto &[id, node] : semantic::expand(scene).nodes) {
        (void)id;
        const auto found =
            std::find_if(physical.nodes.begin(), physical.nodes.end(), [&](const auto &row) {
                return row.second.originalPath == node.originalPath;
            });
        REQUIRE(found != physical.nodes.end());
        CHECK(found->second.worldTransform == node.worldTransform);
        CHECK(found->second.tags == node.tags);
        CHECK(found->second.children.size() == node.children.size());
    }
}
