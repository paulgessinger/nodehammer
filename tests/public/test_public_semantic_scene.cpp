#include <cstring>
#include <fstream>
#include <iterator>
#include <nodehammer/io.hpp>
#include <nodehammer/nhb.hpp>
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

TEST_CASE("readSemantic imports through the synthetic backend", "[public][semantic]") {
    const auto result = nh::readSemantic("", nh::SemanticReadOptions{"synthetic"});
    REQUIRE(result.scene.valid());
    REQUIRE_FALSE(result.diags.hasErrors());

    REQUIRE(result.scene.nodeCount() == 1);
    REQUIRE(result.scene.logVolCount() == 1);
    REQUIRE(result.scene.shapeCount() == 1);
    REQUIRE(result.scene.materialCount() == 1);
}

TEST_CASE("semanticReadFormats reports what this build can read and write", "[public][semantic]") {
    const auto formats = nh::semanticReadFormats();
    REQUIRE_FALSE(formats.empty());

    // Unconditional backends: a build that cannot report these is broken rather
    // than merely minimal.
    REQUIRE(nhtest::listed(formats, "synthetic"));
    REQUIRE(nhtest::listed(formats, "json"));
    REQUIRE(nhtest::listed(formats, "nhb"));
    REQUIRE(nhtest::listed(nh::semanticWriteFormats(), "nhb"));
    REQUIRE_FALSE(nhtest::listed(nh::semanticWriteFormats(), "synthetic"));

    // A view over library-lifetime storage, not a container handed across the
    // boundary for the caller to free.
    REQUIRE(nh::semanticReadFormats().data() == formats.data());
}

TEST_CASE("readSemantic rejects a format this build does not have", "[public][semantic]") {
    // Run time rather than link time, and deliberately so: the format is a
    // value, so nothing earlier could have known (#41 §5).
    bool caught = false;
    try {
        (void)nh::readSemantic("scene.xyz", nh::SemanticReadOptions{"no-such-format"});
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0101");
    }
    REQUIRE(caught);
}

TEST_CASE("readSemantic throws on a file that will not open", "[public][semantic]") {
    bool caught = false;
    try {
        (void)nh::readSemantic("/nodehammer/definitely/not/here.nhb");
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

    const std::vector<std::byte> nhb = nh::toNhb(scene);
    REQUIRE_FALSE(nhb.empty());

    const auto reread = nh::fromNhb(std::span<const std::byte>{nhb});
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
    nh::write(scene, nhbPath); // returns nothing: it wrote the file or it threw
    REQUIRE(std::filesystem::exists(nhbPath));
    REQUIRE(std::filesystem::file_size(nhbPath) > 0);

    const auto reread = nh::readSemantic(nhbPath);
    REQUIRE(reread.scene.nodeCount() == scene.nodeCount());

    // The other unconditional writer, reached by extension. Smoke only — that
    // JSON is well-formed is tests/ir/test_json_roundtrip.cpp's business; what
    // this checks is that the second exporter is reachable at all.
    const auto jsonPath = dir / "scene.json";
    nh::write(scene, jsonPath);
    REQUIRE(std::filesystem::file_size(jsonPath) > 0);
    REQUIRE(nh::readSemantic(jsonPath).scene.nodeCount() == scene.nodeCount());

    // Compression is a suffix, not a format — so `.nhb.zst` still resolves the
    // nhb writer, and reaches zstd on the way.
    const auto zstPath = dir / "scene.nhb.zst";
    nh::write(scene, zstPath);
    REQUIRE(std::filesystem::file_size(zstPath) > 0);
    REQUIRE(nh::readSemantic(zstPath).scene.nodeCount() == scene.nodeCount());
}

TEST_CASE("write honours an explicit format", "[public][semantic]") {
    const nhtest::TempDir dir{"semantic_format"};
    const auto scene = nhtest::syntheticScene();

    // An extension no exporter claims, overridden by the option — which is the
    // only thing WriteOptions::format is for.
    const auto path = dir / "scene.bin";
    nh::write(scene, path, nh::SemanticWriteOptions{"nhb"});
    REQUIRE(std::filesystem::file_size(path) > 0);
    REQUIRE(nh::readSemantic(path, nh::SemanticReadOptions{"nhb"}).scene.nodeCount() ==
            scene.nodeCount());
}

TEST_CASE("write rejects a format this build does not have", "[public][semantic]") {
    const nhtest::TempDir dir{"semantic_bad_format"};
    bool caught = false;
    try {
        nh::write(nhtest::syntheticScene(), dir / "scene.nhb",
                  nh::SemanticWriteOptions{"no-such-format"});
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
        nh::write(empty, "unused.nhb");
    } catch (const nh::Error &e) {
        caughtWrite = true;
        REQUIRE(e.code() == "NH0800");
        REQUIRE(e.context() == "write");
    }
    REQUIRE(caughtWrite);

    bool caughtBytes = false;
    try {
        (void)nh::toNhb(empty);
    } catch (const nh::Error &e) {
        caughtBytes = true;
        REQUIRE(e.code() == "NH0800");
        REQUIRE(e.context() == "toNhb");
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

TEST_CASE("NHB transport and file compression share the selected level", "[public][semantic]") {
    const auto scene = nhtest::syntheticScene();
    const auto dir = std::filesystem::temp_directory_path() / "nh_public_compression";
    std::filesystem::create_directories(dir);
    for (const int level : {-1, 3, 9}) {
        const auto compressed = nh::toNhbZstd(scene, level);
        REQUIRE(nh::toNhb(nh::fromNhb(compressed).scene) == nh::toNhb(scene));
        const auto path = dir / "scene.nhb.zst";
        nh::write(scene, path, {.compressionLevel = level});
        std::ifstream input(path, std::ios::binary);
        const std::vector<char> actual{std::istreambuf_iterator<char>{input}, {}};
        REQUIRE(actual.size() == compressed.size());
        REQUIRE(std::memcmp(actual.data(), compressed.data(), actual.size()) == 0);
        REQUIRE(nh::toNhb(nh::readSemantic(path).scene) == nh::toNhb(scene));
    }
    const auto plain = dir / "scene.nhb";
    nh::write(scene, plain, {.compressionLevel = 9});
    REQUIRE(std::filesystem::file_size(plain) == nh::toNhb(scene).size());
    const auto forced = dir / "scene.data.zst";
    nh::write(scene, forced, {.format = "nhb", .compressionLevel = 9});
    REQUIRE(nh::toNhb(nh::readSemantic(forced, {.format = "nhb"}).scene) == nh::toNhb(scene));
    auto truncated = nh::toNhbZstd(scene);
    truncated.resize(truncated.size() / 2);
    REQUIRE_THROWS_AS(nh::fromNhb(truncated), nh::Error);
    REQUIRE_THROWS_AS(nh::toNhbZstd(nh::SemanticScene{}), nh::Error);
    std::filesystem::remove_all(dir);
}
