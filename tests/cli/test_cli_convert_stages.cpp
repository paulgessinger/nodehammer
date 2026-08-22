// `convert` reaching both ends of the pipeline.
//
// `dump-semantic` and `dump-render` were `convert` with a different hardcoded
// writer: all three imported, applied a config, and wrote — one stopping at the
// semantic scene, two carrying on through tessellation. The output format
// already said which, so the three commands were one command whose name had
// been chosen three times. The worse consequence was `dump-semantic`: writing
// `.nhb` is how a project gets its geometry blob, so the mainstream publishing
// path ran through a verb called "dump".
//
// What is asserted here is the rule that replaced them: the run goes as deep as
// the deepest output asks for, and no deeper.

#include "cli_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>

namespace fs = std::filesystem;

namespace {

class TempDir {
  public:
    TempDir() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / std::format("nh_convert_test_{}", tick);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] std::string at(std::string_view name) const { return (path_ / name).string(); }

  private:
    fs::path path_;
};

} // namespace

TEST_CASE("convert stops at the semantic scene when that is all that was asked for",
          "[cli][convert]") {
    // `--synthetic-box` rather than a fixture, so the case needs nothing on
    // disk and no importer backend.
    TempDir dir;
    const auto target = dir.at("scene.nhb");

    const auto outcome = nhtest::runCaptured({"convert", "--synthetic-box", "--output", target});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    REQUIRE(fs::is_regular_file(target));
    // Tessellation did not run, so nothing counted triangles.
    CHECK(outcome.out.find("Triangles") == std::string::npos);
    CHECK(outcome.out.find("Shapes") != std::string::npos);
}

TEST_CASE("convert tessellates when a render output asks it to", "[cli][convert]") {
    TempDir dir;
    const auto target = dir.at("scene.nhr");

    const auto outcome = nhtest::runCaptured({"convert", "--synthetic-box", "--output", target});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    REQUIRE(fs::is_regular_file(target));
    CHECK(outcome.out.find("Triangles") != std::string::npos);
}

TEST_CASE("two outputs at two depths are one run", "[cli][convert]") {
    // Not two passes over the input: the pipeline runs once, to the deeper of
    // the two, and each output is written as it goes past.
    TempDir dir;
    const auto semantic = dir.at("scene.nhb");
    const auto render = dir.at("scene.nhr");

    const auto outcome = nhtest::runCaptured(
        {"convert", "--synthetic-box", "--output", semantic, "--output", render});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    CHECK(fs::is_regular_file(semantic));
    CHECK(fs::is_regular_file(render));
}

TEST_CASE(".json means the shallower of the two scenes", "[cli][convert]") {
    // Both the semantic and the render scene have a JSON form and both claim
    // the extension. The semantic registry is consulted first, so the bare
    // spelling is the semantic one and the render form is reached by name.
    TempDir dir;

    SECTION("by extension: semantic, so no tessellation") {
        const auto target = dir.at("scene.json");
        const auto outcome =
            nhtest::runCaptured({"convert", "--synthetic-box", "--output", target});
        REQUIRE(outcome.code == 0);
        CHECK(outcome.out.find("Triangles") == std::string::npos);
    }

    SECTION("by name: the render scene, which tessellates") {
        const auto target = dir.at("render.json");
        const auto outcome = nhtest::runCaptured(
            {"convert", "--synthetic-box", "--output-format", "render-json", "--output", target});
        INFO("stderr was: " << outcome.err);
        REQUIRE(outcome.code == 0);
        CHECK(outcome.out.find("Triangles") != std::string::npos);
    }
}

TEST_CASE("an output nothing claims is refused by name", "[cli][convert]") {
    TempDir dir;
    const auto outcome =
        nhtest::runCaptured({"convert", "--synthetic-box", "--output", dir.at("scene.xyz")});

    CHECK(outcome.code != 0);
    CHECK(outcome.mentions("scene.xyz"));
}

TEST_CASE("convert without an input says which flags supply one", "[cli][convert]") {
    TempDir dir;
    const auto outcome = nhtest::runCaptured({"convert", "--output", dir.at("scene.nhb")});

    CHECK(outcome.code != 0);
    CHECK(outcome.mentions("--synthetic-box"));
}

TEST_CASE("the dump commands are gone", "[cli][convert]") {
    for (const auto *name : {"dump-semantic", "dump-render"}) {
        const auto outcome = nhtest::runCaptured({name, "--input", "whatever.nhb"});
        INFO("subcommand: " << name);
        CHECK(outcome.code != 0);
    }
}
