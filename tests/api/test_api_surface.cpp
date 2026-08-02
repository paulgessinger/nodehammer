// The contract the handles themselves make, independent of what the pipeline
// computes: opacity, the value semantics of a shared read-only handle, the
// diagnostics range, and — the one that matters most — that no entry point
// throws or crashes when handed something it cannot use.

#include <catch2/catch_test_macros.hpp>

#include <api/handles.hpp>
#include <detail/file_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/synthetic/semantic/importer.hpp>

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/version.hpp>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
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
    // The observers answer for an empty handle rather than dereferencing; the
    // verbs and the byte forms throw, which is the previous test case.
    REQUIRE(empty.nodeCount() == 0);
    REQUIRE(empty.logVolCount() == 0);
    REQUIRE(empty.shapeCount() == 0);
    REQUIRE(empty.materialCount() == 0);

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
}

TEST_CASE("The state getter throws rather than binding a null reference", "[api][handles]") {
    // The one part of the seam only an in-tree caller can ask about, hence the
    // internal include: `impl()` is undecorated, so it is in no export table,
    // and `Impl` is defined in no installed header. The library's own callers
    // ask `valid()` first — this pins what happens to one that forgets, since
    // that branch is otherwise unreached.
    REQUIRE_THROWS_AS(nh::SemanticScene{}.impl(), nh::Error);
    REQUIRE_THROWS_AS(nh::RenderScene{}.impl(), nh::Error);
    REQUIRE_THROWS_AS(nh::Config{}.impl(), nh::Error);
    REQUIRE_THROWS_AS(nh::SceneConfig{}.impl(), nh::Error);
    REQUIRE_THROWS_AS(nh::OutputConfig{}.impl(), nh::Error);

    // A slice of an empty `Config` is the one case where the two questions
    // differ: it holds state, so it answers `impl()`, but the document behind
    // that state is null — which is what `valid()` reports and what
    // `api::configOf` turns into the built-in defaults.
    const auto slice = nh::Config{}.scene();
    REQUIRE_FALSE(slice.valid());
    REQUIRE(slice.impl().cfg == nullptr);
}

TEST_CASE("DiagnosticList is an ordered range that survives being moved from", "[api][handles]") {
    // A scene that tessellates with something to say: the unknown shape reports
    // NH0500 and the node comes back unmeshed, which is the case the diagnostic
    // channel exists for — a result *and* a complaint about it.
    const auto imported = nh::SemanticScene::read("", nh::SemanticScene::ReadOptions{"synthetic"});
    const auto rendered = nh::build(imported.scene, {});
    REQUIRE(rendered.scene.valid());
    REQUIRE_FALSE(rendered.diags.empty());

    std::size_t seen = 0;
    for (const auto &d : rendered.diags) {
        REQUIRE_FALSE(d.code.empty());
        ++seen;
    }
    REQUIRE(seen == rendered.diags.size());

    nh::DiagnosticList copy = rendered.diags;
    REQUIRE(copy.size() == rendered.diags.size());
    REQUIRE(copy.begin()->code == rendered.diags.begin()->code);
    const auto &missing = rendered;

    // A moved-from list is an empty range rather than a trap: begin() == end()
    // and every accessor still answers.
    const nh::DiagnosticList moved = std::move(copy);
    REQUIRE(moved.size() == missing.diags.size());
    REQUIRE(copy.empty());               // NOLINT(bugprone-use-after-move) — the point of the test
    REQUIRE(copy.size() == 0);           // NOLINT(bugprone-use-after-move)
    REQUIRE_FALSE(copy.hasErrors());     // NOLINT(bugprone-use-after-move)
    REQUIRE(copy.begin() == copy.end()); // NOLINT(bugprone-use-after-move)

    const nh::DiagnosticList fresh;
    REQUIRE(fresh.empty());
    REQUIRE(fresh.begin() == fresh.end());
    REQUIRE_FALSE(fresh.hasErrors());
}

TEST_CASE("Input the API cannot act on throws Error", "[api][handles]") {
    const auto dir = caseDir("errors");

    // A format no backend claims. Necessarily a run-time failure rather than a
    // link-time one: the format is a value, so nothing earlier could know.
    REQUIRE_THROWS_AS(
        nh::SemanticScene::read(dir / "x.nhb", nh::SemanticScene::ReadOptions{"not-a-format"}),
        nh::Error);
    REQUIRE_THROWS_AS(nh::SemanticScene::read(dir / "x.wat"), nh::Error);

    // A file that will not open.
    REQUIRE_THROWS_AS(nh::SemanticScene::read(dir / "nope.nhb"), nh::Error);

    // Bytes that are not a scene: the FlatBuffers verifier throws internally,
    // and only `Error` may reach the caller.
    const std::vector<std::byte> garbage(64, std::byte{0x7f});
    REQUIRE_THROWS_AS(nh::SemanticScene::read(std::span{garbage}), nh::Error);
    REQUIRE_THROWS_AS(nh::RenderScene::read(std::span{garbage}), nh::Error);

    // A handle that refers to nothing, on every entry point that takes one.
    const nh::RenderScene emptyRender;
    const nh::SemanticScene emptySemantic;
    REQUIRE_THROWS_AS(emptyRender.write(dir / "out.glb"), nh::Error);
    REQUIRE_THROWS_AS(emptySemantic.write(dir / "out.nhb"), nh::Error);
    REQUIRE_THROWS_AS(emptySemantic.toNhb(), nh::Error);
    REQUIRE_THROWS_AS(emptyRender.toNhr(), nh::Error);
    REQUIRE_THROWS_AS(nh::applySelection(emptySemantic, {}), nh::Error);
    REQUIRE_THROWS_AS(nh::deduplicate(emptySemantic, {}), nh::Error);
    REQUIRE_THROWS_AS(nh::tessellate(emptySemantic, {}), nh::Error);
    REQUIRE_THROWS_AS(nh::build(emptySemantic, {}), nh::Error);

    // An unwritable destination.
    const auto scene = boxScene();
    REQUIRE_THROWS_AS(scene.write(fs::path{"/definitely/not/here/out.nhb"}), nh::Error);
}

TEST_CASE("Error carries a code, a context and its Diagnostic form", "[api][handles]") {
    const auto dir = caseDir("error_payload");
    try {
        (void)nh::SemanticScene::read(dir / "nope.nhb");
        FAIL("expected a throw");
    } catch (const nh::Error &e) {
        REQUIRE(e.code() == nh::codes::kErrImportFileNotFound);
        REQUIRE_FALSE(std::string_view{e.what()}.empty());
        REQUIRE_FALSE(e.context().empty());
        const auto d = e.diagnostic();
        REQUIRE(d.severity == nh::Diagnostic::Severity::Error);
        REQUIRE(d.code == e.code());
        REQUIRE(d.message == e.what());
    }

    // Catchable as a plain std::exception, so a caller that wants one handler
    // for everything gets the message without knowing this type.
    REQUIRE_THROWS_AS(nh::SemanticScene::read(dir / "nope.nhb"), std::exception);
}

TEST_CASE("formats() is the runtime capability query", "[api][handles]") {
    const auto semantic = nh::SemanticScene::formats();
    REQUIRE(std::ranges::find(semantic, "flatbuffer") != semantic.end());
    REQUIRE(std::ranges::find(semantic, "json") != semantic.end());
    REQUIRE(std::ranges::find(semantic, "synthetic") != semantic.end());
    // No duplicates: the importer and exporter registries overlap.
    std::vector<std::string_view> sortedSemantic{semantic.begin(), semantic.end()};
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

    // A view over library-lifetime storage, so two calls see the same bytes
    // rather than two freshly-built containers.
    REQUIRE(nh::RenderScene::formats().data() == render.data());
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
