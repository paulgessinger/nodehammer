#include <fstream>
#include <iterator>
#include <nodehammer/io.hpp>
#include <nodehammer/nhr.hpp>
// `RenderScene`, through the shared library.
//
// Triangle counts are compared against each other rather than pinned to a
// number: what the tessellator makes of a box belongs to
// tests/tessellation/, and pinning it here would turn a deliberate change to
// the mesher into a failure in the packaging suite.

#include "public_fixture.hpp"

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/render_scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace nh = nodehammer;

namespace {

/// The one tessellated scene these cases share, built through the public verbs
/// from the synthetic box.
nh::RenderScene rendered() {
    auto result = nh::build(nhtest::syntheticScene(), nh::SceneConfig{});
    REQUIRE_FALSE(result.diags.hasErrors());
    return std::move(result.scene);
}

} // namespace

TEST_CASE("a tessellated scene reports consistent counts", "[public][render]") {
    const auto scene = rendered();
    REQUIRE(scene.valid());

    REQUIRE(scene.triangleCount() > 0);
    // A scene with triangles has meshes to hold them and materials to bind
    // them. That is the invariant; the numbers themselves are the mesher's.
    REQUIRE(scene.meshCount() > 0);
    REQUIRE(scene.materialCount() > 0);
    REQUIRE(scene.nodeCount() == 1); // one box in, one node out
}

TEST_CASE("renderWriteFormats reports what this build can read and write", "[public][render]") {
    const auto formats = nh::renderWriteFormats();
    REQUIRE_FALSE(formats.empty());
    REQUIRE(nhtest::listed(formats, "nhr"));
    REQUIRE(nhtest::listed(formats, "gltf"));
    REQUIRE(nhtest::listed(formats, "obj"));

    REQUIRE(nh::renderWriteFormats().data() == formats.data());
}

TEST_CASE("RenderScene round-trips through .nhr bytes", "[public][render]") {
    const auto scene = rendered();

    const std::vector<std::byte> nhr = nh::toNhr(scene);
    REQUIRE_FALSE(nhr.empty());

    const auto reread = nh::fromNhr(std::span<const std::byte>{nhr});
    REQUIRE(reread.valid());
    REQUIRE(reread.nodeCount() == scene.nodeCount());
    REQUIRE(reread.meshCount() == scene.meshCount());
    REQUIRE(reread.materialCount() == scene.materialCount());
    REQUIRE(reread.triangleCount() == scene.triangleCount());
}

TEST_CASE("RenderScene round-trips through a file", "[public][render]") {
    const nhtest::TempDir dir{"render_roundtrip"};
    const auto scene = rendered();

    const auto path = dir / "scene.nhr";
    nh::write(scene, path);
    REQUIRE(std::filesystem::file_size(path) > 0);

    const auto reread = nh::readRender(path);
    REQUIRE(reread.triangleCount() == scene.triangleCount());

    // And through the compressed suffix, which reaches zstd.
    const auto zstPath = dir / "scene.nhr.zst";
    nh::write(scene, zstPath);
    REQUIRE(std::filesystem::file_size(zstPath) > 0);
    REQUIRE(nh::readRender(zstPath).triangleCount() == scene.triangleCount());
}

TEST_CASE("every render exporter is reachable", "[public][render]") {
    // Smoke, and deliberately so: whether the glTF is well-formed is
    // tests/export/test_gltf_exporter.cpp's question, asked there against the
    // archive with tinygltf on hand to read the result back. What is asked here
    // is only whether the writer exists on this side of the boundary — glTF
    // reaches tinygltf, which a consumer never links, so "the file has bytes in
    // it" is the claim that matters.
    const nhtest::TempDir dir{"render_exporters"};
    const auto scene = rendered();

    for (const char *leaf : {"scene.gltf", "scene.glb", "scene.obj", "scene.nhr"}) {
        const auto path = dir / leaf;
        nh::write(scene, path);
        INFO("wrote " << path.string());
        REQUIRE(std::filesystem::exists(path));
        REQUIRE(std::filesystem::file_size(path) > 0);
    }
}

TEST_CASE("write takes its tuning from an OutputConfig", "[public][render]") {
    // `OutputConfig` reaches `write` and nowhere else, so this is the only
    // place the slice is exercised as an argument rather than as a value.
    const nhtest::TempDir dir{"render_output_config"};
    const auto scene = rendered();
    const auto config = nh::fromToml("[export.gltf]\nunit_scale = 0.1\n");
    REQUIRE(config.config.output().valid());

    const auto tuned = dir / "tuned.gltf";
    nh::write(scene, tuned, config.config.output());
    REQUIRE(std::filesystem::file_size(tuned) > 0);

    // A default-constructed slice means each format's built-in defaults, so the
    // same call with no config has to work too.
    const auto plain = dir / "plain.gltf";
    nh::write(scene, plain, nh::OutputConfig{});
    REQUIRE(std::filesystem::file_size(plain) > 0);

    // And the explicit-format option, against an extension no writer claims.
    const auto forced = dir / "forced.bin";
    nh::write(scene, forced, nh::OutputConfig{}, nh::RenderWriteOptions{"nhr"});
    REQUIRE(nh::readRender(forced).triangleCount() == scene.triangleCount());
}

TEST_CASE("write rejects a format this build does not have", "[public][render]") {
    const nhtest::TempDir dir{"render_bad_format"};
    bool caught = false;
    try {
        nh::write(rendered(), dir / "scene.nhr", nh::OutputConfig{},
                  nh::RenderWriteOptions{"no-such-format"});
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0600");
    }
    REQUIRE(caught);
}

TEST_CASE("readRender throws on a file that will not open", "[public][render]") {
    bool caught = false;
    try {
        (void)nh::readRender("/nodehammer/definitely/not/here.nhr");
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0100");
    }
    REQUIRE(caught);
}

TEST_CASE("readRender rejects bytes that are not a render scene", "[public][render]") {
    const std::vector<std::byte> garbage(64, std::byte{0x7f});
    bool caught = false;
    try {
        (void)nh::fromNhr(std::span<const std::byte>{garbage});
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0100");
    }
    REQUIRE(caught);
}

TEST_CASE("an empty RenderScene answers, and throws where it would dereference",
          "[public][render]") {
    const nh::RenderScene empty;
    REQUIRE_FALSE(empty.valid());
    REQUIRE(empty.nodeCount() == 0);
    REQUIRE(empty.meshCount() == 0);
    REQUIRE(empty.materialCount() == 0);
    REQUIRE(empty.triangleCount() == 0);

    bool caughtWrite = false;
    try {
        nh::write(empty, "unused.nhr");
    } catch (const nh::Error &e) {
        caughtWrite = true;
        REQUIRE(e.code() == "NH0800");
    }
    REQUIRE(caughtWrite);

    bool caughtBytes = false;
    try {
        (void)nh::toNhr(empty);
    } catch (const nh::Error &e) {
        caughtBytes = true;
        REQUIRE(e.code() == "NH0800");
    }
    REQUIRE(caughtBytes);
}

TEST_CASE("render compression follows the filename and rejects unsupported formats",
          "[public][render]") {
    const auto scene = rendered();
    const auto dir = std::filesystem::temp_directory_path() / "nh_render_compression";
    std::filesystem::create_directories(dir);
    const auto path = dir / "scene.nhr.zst";
    nh::write(scene, path, {}, {.compressionLevel = 9});
    REQUIRE(nh::toNhr(nh::readRender(path)) == nh::toNhr(scene));
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>{input}, {}};
    REQUIRE(nh::toNhr(nh::fromNhr(std::as_bytes(std::span{bytes}))) == nh::toNhr(scene));
    REQUIRE_THROWS_AS(nh::write(scene, dir / "scene.glb.zst", {}, {.format = "gltf"}), nh::Error);
    REQUIRE(nh::renderReadFormats().size() == 1);
    REQUIRE(nh::renderReadFormats()[0] == "nhr");
    REQUIRE_FALSE(nhtest::listed(nh::renderReadFormats(), "gltf"));
    std::filesystem::remove_all(dir);
}
