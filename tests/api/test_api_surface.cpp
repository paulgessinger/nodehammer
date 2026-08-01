// The contract the handles themselves make, independent of what the pipeline
// computes: opacity, the value semantics of a shared read-only handle, the
// diagnostics range, and — the one that matters most — that no entry point
// throws or crashes when handed something it cannot use.

#include <catch2/catch_test_macros.hpp>

#include <detail/file_io.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/synthetic/semantic/importer.hpp>

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/version.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace nh = nodehammer;

fs::path caseDir(std::string_view name) {
    const auto dir = fs::temp_directory_path() / "nh_api_surface" / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

nh::SemanticScene boxScene() {
    const auto dir = caseDir("box");
    const auto nhb = dir / "box.nhb";
    nh::detail::file_io::writeFile(
        nhb, nh::ir::semanticSceneToBytes(nh::ir::SyntheticSceneBuilder::buildSingleBox()));
    auto result = nh::SemanticScene::read(nhb);
    REQUIRE(result.scene.valid());
    return std::move(result.scene);
}

} // namespace

TEST_CASE("Handles are copyable values that share one immutable scene", "[api][handles]") {
    static_assert(std::is_copy_constructible_v<nh::SemanticScene>);
    static_assert(std::is_copy_constructible_v<nh::RenderScene>);
    static_assert(std::is_copy_constructible_v<nh::Config>);
    static_assert(std::is_nothrow_move_constructible_v<nh::SemanticScene>);

    const nh::SemanticScene empty;
    REQUIRE_FALSE(empty.valid());
    REQUIRE(empty.nodeCount() == 0);
    REQUIRE(empty.logVolCount() == 0);
    REQUIRE(empty.shapeCount() == 0);
    REQUIRE(empty.materialCount() == 0);
    // Everything is answerable on an invalid handle; nothing dereferences.
    REQUIRE(empty.toNhb().empty());

    const auto scene = boxScene();
    const auto copy = scene;
    REQUIRE(copy.valid());
    REQUIRE(copy.nodeCount() == scene.nodeCount());
    REQUIRE(copy.toNhb() == scene.toNhb());

    const nh::RenderScene emptyRender;
    REQUIRE_FALSE(emptyRender.valid());
    REQUIRE(emptyRender.nodeCount() == 0);
    REQUIRE(emptyRender.meshCount() == 0);
    REQUIRE(emptyRender.materialCount() == 0);
    REQUIRE(emptyRender.triangleCount() == 0);
    REQUIRE(emptyRender.toNhr().empty());
}

TEST_CASE("DiagnosticList is an ordered range that survives being moved from", "[api][handles]") {
    const auto missing = nh::SemanticScene::read(fs::path{"/definitely/not/here.nhb"});
    REQUIRE(missing.diags.hasErrors());
    REQUIRE_FALSE(missing.diags.empty());
    REQUIRE(missing.diags.size() == 1);

    std::size_t seen = 0;
    for (const auto &d : missing.diags) {
        REQUIRE_FALSE(d.code.empty());
        REQUIRE(d.severity >= nh::Diagnostic::Severity::Error);
        ++seen;
    }
    REQUIRE(seen == missing.diags.size());

    nh::DiagnosticList copy = missing.diags;
    REQUIRE(copy.size() == missing.diags.size());
    REQUIRE(copy.begin()->code == missing.diags.begin()->code);

    // A moved-from list is an empty range rather than a trap: begin() == end()
    // and every accessor still answers.
    const nh::DiagnosticList moved = std::move(copy);
    REQUIRE(moved.size() == 1);
    REQUIRE(copy.empty());               // NOLINT(bugprone-use-after-move) — the point of the test
    REQUIRE(copy.size() == 0);           // NOLINT(bugprone-use-after-move)
    REQUIRE_FALSE(copy.hasErrors());     // NOLINT(bugprone-use-after-move)
    REQUIRE(copy.begin() == copy.end()); // NOLINT(bugprone-use-after-move)

    const nh::DiagnosticList fresh;
    REQUIRE(fresh.empty());
    REQUIRE(fresh.begin() == fresh.end());
    REQUIRE_FALSE(fresh.hasErrors());
}

TEST_CASE("Nothing throws across the boundary", "[api][handles]") {
    const auto dir = caseDir("errors");

    // A format this build does not have is a diagnostic, not a crash and not an
    // exception — the string-dispatched entry points cannot fail any earlier.
    const auto unknownFormat =
        nh::SemanticScene::read(dir / "x.nhb", nh::SemanticScene::ReadOptions{"not-a-format"});
    REQUIRE(unknownFormat.diags.hasErrors());
    REQUIRE_FALSE(unknownFormat.scene.valid());

    const auto unknownExtension = nh::SemanticScene::read(dir / "x.wat");
    REQUIRE(unknownExtension.diags.hasErrors());

    // Garbage bytes reach a FlatBuffers verifier that throws internally.
    const std::vector<std::byte> garbage(64, std::byte{0x7f});
    const auto badSemantic = nh::SemanticScene::read(std::span{garbage});
    REQUIRE(badSemantic.diags.hasErrors());
    const auto badRender = nh::RenderScene::read(std::span{garbage});
    REQUIRE(badRender.diags.hasErrors());
    REQUIRE_FALSE(badRender.scene.valid());

    // Writing an invalid handle reports rather than dereferences.
    const nh::RenderScene emptyRender;
    REQUIRE(emptyRender.write(dir / "out.glb").hasErrors());
    const nh::SemanticScene emptySemantic;
    REQUIRE(emptySemantic.write(dir / "out.nhb").hasErrors());

    // So does every verb.
    REQUIRE(nh::applySelection(emptySemantic, {}).diags.hasErrors());
    REQUIRE(nh::deduplicate(emptySemantic, {}).diags.hasErrors());
    REQUIRE(nh::tessellate(emptySemantic, {}).diags.hasErrors());
    REQUIRE(nh::build(emptySemantic, {}).diags.hasErrors());

    // Unwritable destination.
    const auto scene = boxScene();
    REQUIRE(scene.write(fs::path{"/definitely/not/here/out.nhb"}).hasErrors());
}

TEST_CASE("formats() is the runtime capability query", "[api][handles]") {
    const auto semantic = nh::SemanticScene::formats();
    REQUIRE(std::ranges::find(semantic, "flatbuffer") != semantic.end());
    REQUIRE(std::ranges::find(semantic, "json") != semantic.end());
    REQUIRE(std::ranges::find(semantic, "synthetic") != semantic.end());
    // No duplicates: the importer and exporter registries overlap.
    auto sortedSemantic = semantic;
    std::ranges::sort(sortedSemantic);
    REQUIRE(std::ranges::adjacent_find(sortedSemantic) == sortedSemantic.end());

#if NH_WITH_TGEO
    REQUIRE(std::ranges::find(semantic, "tgeo") != semantic.end());
#else
    REQUIRE(std::ranges::find(semantic, "tgeo") == semantic.end());
#endif

    const auto render = nh::RenderScene::formats();
    for (const auto *name : {"nhr", "gltf", "obj"}) {
        REQUIRE(std::ranges::find(render, name) != render.end());
    }
}

TEST_CASE("The scene handles round-trip through their own byte forms", "[api][handles]") {
    const auto dir = caseDir("roundtrip");
    const auto scene = boxScene();

    const auto bytes = scene.toNhb();
    REQUIRE_FALSE(bytes.empty());
    const auto reread = nh::SemanticScene::read(std::span{bytes});
    REQUIRE_FALSE(reread.diags.hasErrors());
    REQUIRE(reread.scene.nodeCount() == scene.nodeCount());
    REQUIRE(reread.scene.shapeCount() == scene.shapeCount());

    // ... and through a file, in both formats the semantic exporters claim.
    for (const auto *ext : {".nhb", ".json"}) {
        const auto path = dir / ("scene" + std::string{ext});
        REQUIRE_FALSE(scene.write(path).hasErrors());
        REQUIRE(fs::exists(path));
        const auto loaded = nh::SemanticScene::read(path);
        REQUIRE_FALSE(loaded.diags.hasErrors());
        REQUIRE(loaded.scene.nodeCount() == scene.nodeCount());
    }

    const auto rendered = nh::build(scene, {});
    REQUIRE(rendered.scene.valid());
    REQUIRE(rendered.scene.triangleCount() > 0);

    const auto nhr = dir / "scene.nhr";
    REQUIRE_FALSE(rendered.scene.write(nhr).hasErrors());
    const auto reloaded = nh::RenderScene::read(nhr);
    REQUIRE_FALSE(reloaded.diags.hasErrors());
    REQUIRE(reloaded.scene.triangleCount() == rendered.scene.triangleCount());
    REQUIRE(reloaded.scene.toNhr() == rendered.scene.toNhr());

    // The compressed spelling is the same format, so the extension check has to
    // see through the `.zst` suffix rather than only the last extension.
    const auto compressed = dir / "scene.nhr.zst";
    REQUIRE_FALSE(rendered.scene.write(compressed).hasErrors());
    REQUIRE(fs::file_size(compressed) > 0);
    const auto fromZst = nh::RenderScene::read(compressed);
    REQUIRE_FALSE(fromZst.diags.hasErrors());
    REQUIRE(fromZst.scene.toNhr() == rendered.scene.toNhr());
}

TEST_CASE("version() reports the linked library, not just the header", "[api][handles]") {
    REQUIRE(nh::version() == nh::VERSION);
}
