// `SemanticScene`, through the shared library.
//
// The counts are exact rather than merely non-zero: the synthetic importer
// builds one box — one node, one logical volume, one shape, one material — so a
// count that came back wrong is a count that did not survive the boundary,
// which is a different failure from a pipeline bug and worth telling apart.

#include "public_fixture.hpp"

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <utility>
#include <vector>

namespace nh = nodehammer;

TEST_CASE("SemanticScene::read imports through the synthetic backend", "[public][semantic]") {
    const auto result = nh::SemanticScene::read("", nh::SemanticScene::ReadOptions{"synthetic"});
    REQUIRE(result.scene.valid());
    REQUIRE_FALSE(result.diags.hasErrors());

    REQUIRE(result.scene.nodeCount() == 1);
    REQUIRE(result.scene.logVolCount() == 1);
    REQUIRE(result.scene.shapeCount() == 1);
    REQUIRE(result.scene.materialCount() == 1);
}

TEST_CASE("SemanticScene::formats reports what this build can read and write",
          "[public][semantic]") {
    const auto formats = nh::SemanticScene::formats();
    REQUIRE_FALSE(formats.empty());

    // Unconditional backends: a build that cannot report these is broken rather
    // than merely minimal.
    REQUIRE(nhtest::listed(formats, "synthetic"));
    REQUIRE(nhtest::listed(formats, "json"));
    REQUIRE(nhtest::listed(formats, "flatbuffer"));
    REQUIRE(nhtest::listed(formats, "nhb")); // the write side's name for it

    // A view over library-lifetime storage, not a container handed across the
    // boundary for the caller to free.
    REQUIRE(nh::SemanticScene::formats().data() == formats.data());
}

TEST_CASE("SemanticScene::read rejects a format this build does not have", "[public][semantic]") {
    // Run time rather than link time, and deliberately so: the format is a
    // value, so nothing earlier could have known (#41 §5).
    bool caught = false;
    try {
        (void)nh::SemanticScene::read("scene.xyz",
                                      nh::SemanticScene::ReadOptions{"no-such-format"});
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0101");
    }
    REQUIRE(caught);
}

TEST_CASE("SemanticScene::read throws on a file that will not open", "[public][semantic]") {
    bool caught = false;
    try {
        (void)nh::SemanticScene::read("/nodehammer/definitely/not/here.nhb");
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0100");
        REQUIRE_FALSE(e.context().empty()); // the path it could not open
    }
    REQUIRE(caught);
}

TEST_CASE("SemanticScene round-trips through .nhb bytes", "[public][semantic]") {
    // The path that reaches flatbuffers — one of the static dependencies the
    // shared object has to have absorbed, since a consumer links none of them.
    const auto scene = nhtest::syntheticScene();

    const std::vector<std::byte> nhb = scene.toNhb();
    REQUIRE_FALSE(nhb.empty());

    const auto reread = nh::SemanticScene::read(std::span<const std::byte>{nhb});
    REQUIRE(reread.scene.valid());
    REQUIRE(reread.scene.nodeCount() == scene.nodeCount());
    REQUIRE(reread.scene.logVolCount() == scene.logVolCount());
    REQUIRE(reread.scene.shapeCount() == scene.shapeCount());
    REQUIRE(reread.scene.materialCount() == scene.materialCount());
}

TEST_CASE("SemanticScene round-trips through a file", "[public][semantic]") {
    const nhtest::TempDir dir{"semantic_roundtrip"};
    const auto scene = nhtest::syntheticScene();

    const auto nhbPath = dir / "scene.nhb";
    scene.write(nhbPath); // returns nothing: it wrote the file or it threw
    REQUIRE(std::filesystem::exists(nhbPath));
    REQUIRE(std::filesystem::file_size(nhbPath) > 0);

    const auto reread = nh::SemanticScene::read(nhbPath);
    REQUIRE(reread.scene.nodeCount() == scene.nodeCount());

    // The other unconditional writer, reached by extension. Smoke only — that
    // JSON is well-formed is tests/ir/test_json_roundtrip.cpp's business; what
    // this checks is that the second exporter is reachable at all.
    const auto jsonPath = dir / "scene.json";
    scene.write(jsonPath);
    REQUIRE(std::filesystem::file_size(jsonPath) > 0);
    REQUIRE(nh::SemanticScene::read(jsonPath).scene.nodeCount() == scene.nodeCount());

    // Compression is a suffix, not a format — so `.nhb.zst` still resolves the
    // nhb writer, and reaches zstd on the way.
    const auto zstPath = dir / "scene.nhb.zst";
    scene.write(zstPath);
    REQUIRE(std::filesystem::file_size(zstPath) > 0);
    REQUIRE(nh::SemanticScene::read(zstPath).scene.nodeCount() == scene.nodeCount());
}

TEST_CASE("SemanticScene::write honours an explicit format", "[public][semantic]") {
    const nhtest::TempDir dir{"semantic_format"};
    const auto scene = nhtest::syntheticScene();

    // An extension no exporter claims, overridden by the option — which is the
    // only thing WriteOptions::format is for.
    const auto path = dir / "scene.bin";
    scene.write(path, nh::SemanticScene::WriteOptions{"nhb"});
    REQUIRE(std::filesystem::file_size(path) > 0);
    REQUIRE(nh::SemanticScene::read(path, nh::SemanticScene::ReadOptions{"flatbuffer"})
                .scene.nodeCount() == scene.nodeCount());
}

TEST_CASE("SemanticScene::write rejects a format this build does not have", "[public][semantic]") {
    const nhtest::TempDir dir{"semantic_bad_format"};
    bool caught = false;
    try {
        nhtest::syntheticScene().write(dir / "scene.nhb",
                                       nh::SemanticScene::WriteOptions{"no-such-format"});
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0600");
    }
    REQUIRE(caught);
}

TEST_CASE("an empty SemanticScene answers, and throws where it would dereference",
          "[public][semantic]") {
    const nh::SemanticScene empty;
    REQUIRE_FALSE(empty.valid());

    // The observers answer for an empty handle — they read the state rather
    // than going through it.
    REQUIRE(empty.nodeCount() == 0);
    REQUIRE(empty.logVolCount() == 0);
    REQUIRE(empty.shapeCount() == 0);
    REQUIRE(empty.materialCount() == 0);

    // Everything that would have to dereference one throws, naming the verb the
    // caller got wrong rather than only the type.
    bool caughtWrite = false;
    try {
        empty.write("unused.nhb");
    } catch (const nh::Error &e) {
        caughtWrite = true;
        REQUIRE(e.code() == "NH0800");
        REQUIRE(e.context() == "SemanticScene::write");
    }
    REQUIRE(caughtWrite);

    bool caughtBytes = false;
    try {
        (void)empty.toNhb();
    } catch (const nh::Error &e) {
        caughtBytes = true;
        REQUIRE(e.code() == "NH0800");
        REQUIRE(e.context() == "SemanticScene::toNhb");
    }
    REQUIRE(caughtBytes);
}

TEST_CASE("a SemanticScene handle is cheap to copy and refers to the same scene",
          "[public][semantic]") {
    const auto scene = nhtest::syntheticScene();
    nh::SemanticScene copy = scene;
    REQUIRE(copy.valid());
    REQUIRE(copy.nodeCount() == scene.nodeCount());

    const nh::SemanticScene moved = std::move(copy);
    REQUIRE(moved.valid());
    REQUIRE(moved.nodeCount() == scene.nodeCount());

    // The other way `valid()` can be false, and the reason the observers have
    // to answer for an empty handle at all.
    REQUIRE_FALSE(copy.valid()); // NOLINT(bugprone-use-after-move)
    REQUIRE(scene.valid());      // the original still refers to the scene
}
