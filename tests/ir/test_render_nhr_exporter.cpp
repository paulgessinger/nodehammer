// `.nhr` as a registered exporter rather than a special case above the registry.
//
// The bug this closes was not that `.nhr` could not be written — the API had
// always written it — but that it was written by a branch inside
// `RenderScene::write` that ran *ahead* of the registry. So the registry did not
// know the format existed, `convert` resolved its outputs through the registry,
// and `nodehammer convert -o scene.nhr` failed while
// `nh.RenderScene.write("scene.nhr")` succeeded. Two front doors onto one object
// disagreeing about what it can do.
//
// The assertion that matters is therefore about *reachability through the
// registry*, not about the bytes: the codec already has its own round-trip
// coverage next door.

#include <detail/zstd_io.hpp>
#include <ir/fb/render/flatbuffer.hpp>
#include <ir/render/exporter.hpp>

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/render_scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>

using namespace nodehammer::ir;
namespace fs = std::filesystem;

namespace {

render::Scene makeTinyScene() {
    render::Scene scene;
    const auto rootId = scene.nextNodeId();
    scene.rootId = rootId;
    render::Node node;
    node.id = rootId; // the codec keys on this, not on the map slot
    node.name = "root";
    scene.nodes[rootId] = node;
    return scene;
}

fs::path tempTarget(std::string_view suffix) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / std::format("nh_nhr_{}{}", tick, suffix);
}

} // namespace

TEST_CASE("the registry resolves .nhr, by extension and by name", "[ir][render][nhr]") {
    const auto reg = RenderExporterRegistry::makeDefault();

    SECTION("plain") {
        const auto *exp = reg.resolve("scene.nhr", "");
        REQUIRE(exp != nullptr);
        CHECK(exp->formatName() == "nhr");
    }
    SECTION("compound, and not whoever might claim .zst") {
        const auto *exp = reg.resolve("scene.nhr.zst", "");
        REQUIRE(exp != nullptr);
        CHECK(exp->formatName() == "nhr");
    }
    SECTION("by explicit format name — what --output-format passes") {
        const auto *exp = reg.resolve("output.bin", "nhr");
        REQUIRE(exp != nullptr);
        CHECK(exp->formatName() == "nhr");
    }
}

TEST_CASE("what the registry reports and what the API reports are one list", "[ir][render][nhr]") {
    // The disagreement this file exists to prevent, asserted directly:
    // `RenderScene::formats()` used to prepend "nhr" by hand because the
    // registry had never heard of it.
    const auto reg = RenderExporterRegistry::makeDefault();
    const auto api = nodehammer::RenderScene::formats();

    for (const auto &exp : reg.exporters()) {
        const auto name = exp->formatName();
        CHECK(std::ranges::find(api, name) != api.end());
    }
    CHECK(std::ranges::find(api, std::string_view{"nhr"}) != api.end());
    CHECK(api.size() == reg.exporters().size());
}

TEST_CASE("writing through the registry produces a readable scene", "[ir][render][nhr]") {
    const auto reg = RenderExporterRegistry::makeDefault();
    const auto scene = makeTinyScene();

    SECTION("uncompressed") {
        const auto target = tempTarget(".nhr");
        const auto *exp = reg.resolve(target, "");
        REQUIRE(exp != nullptr);
        exp->write(scene, target, ExportConfig{});

        const auto bytes = nodehammer::detail::zstd_io::readBytesFromFile(target);
        const auto restored = renderSceneFromBytes(bytes);
        CHECK(restored.nodes.at(restored.rootId).name == "root");
        fs::remove(target);
    }

    SECTION("compressed, decided by the path and not by a flag") {
        const auto target = tempTarget(".nhr.zst");
        const auto *exp = reg.resolve(target, "");
        REQUIRE(exp != nullptr);
        exp->write(scene, target, ExportConfig{});

        // readBytesFromFile applies the same suffix convention in reverse; a
        // file that was not actually compressed would fail to decode here.
        const auto bytes = nodehammer::detail::zstd_io::readBytesFromFile(target);
        const auto restored = renderSceneFromBytes(bytes);
        CHECK(restored.nodes.at(restored.rootId).name == "root");
        fs::remove(target);
    }
}

// `convert` calls the exporter directly, with no translating catch of its own,
// and its backstop catches only `nodehammer::Error` — so an exporter that let a
// plain `std::runtime_error` out would escape the NH0600 diagnostic and the
// exit code that goes with it. Every other exporter translates at its own
// boundary; this one has to as well.
TEST_CASE("a write that cannot open its file reports NH0600", "[ir][render][nhr]") {
    const auto reg = RenderExporterRegistry::makeDefault();
    const auto scene = makeTinyScene();
    const fs::path target = "/nodehammer/definitely/not/here.nhr";

    const auto *exp = reg.resolve(target, "");
    REQUIRE(exp != nullptr);

    bool caught = false;
    try {
        exp->write(scene, target, ExportConfig{});
    } catch (const nodehammer::Error &e) {
        caught = true;
        CHECK(e.code() == "NH0600");
    }
    CHECK(caught);
}
