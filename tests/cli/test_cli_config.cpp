// The `config` group.
//
// `validate-config`, `config-flatten` and `config-lua` were three commands, and
// the last two had the same options, the same output format, and — by their own
// help text — the same promise: flattened TOML, round-trippable through
// `ConfigLoader`. What separated them was the input language, which the file
// extension already states, so naming it in the command meant the user had to
// say what was already written down.
//
// The assertion that carries the merge is therefore that one command reaches
// both loaders and produces the same *kind* of document either way.

#include "cli_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class TempDir {
  public:
    TempDir() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / std::format("nh_config_test_{}", tick);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] std::string write(std::string_view name, std::string_view content) const {
        const auto target = path_ / name;
        std::ofstream out(target);
        out << content;
        return target.string();
    }
    [[nodiscard]] std::string at(std::string_view name) const { return (path_ / name).string(); }

  private:
    fs::path path_;
};

} // namespace

TEST_CASE("config flatten inlines a TOML include chain", "[cli][config]") {
    TempDir dir;
    dir.write("base.toml", "hoist_orphans = true\n");
    const auto entry = dir.write("scene.toml", "include = \"base.toml\"\n");

    const auto outcome = nhtest::runCaptured({"config", "flatten", "--config", entry});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    // The included value is present in the output, which is the whole point of
    // flattening: what comes back needs no companion files.
    CHECK(outcome.out.find("hoist_orphans") != std::string::npos);
    CHECK(outcome.out.find("include") == std::string::npos);
}

TEST_CASE("config flatten evaluates a Lua script: same command, same output", "[cli][config]") {
    // This is the merge, asserted: the only thing that changed is the
    // extension, and the command did not.
    TempDir dir;
    const auto entry = dir.write("scene.lua", "config { hoist_orphans = true }\n");

    const auto outcome = nhtest::runCaptured({"config", "flatten", "--config", entry});

    INFO("stderr was: " << outcome.err);
    REQUIRE(outcome.code == 0);
    CHECK(outcome.out.find("hoist_orphans") != std::string::npos);
}

TEST_CASE("config flatten writes to a path when given one", "[cli][config]") {
    TempDir dir;
    const auto entry = dir.write("scene.toml", "hoist_orphans = true\n");
    const auto target = dir.at("flat.toml");

    const auto outcome =
        nhtest::runCaptured({"config", "flatten", "--config", entry, "--output", target});

    REQUIRE(outcome.code == 0);
    REQUIRE(fs::is_regular_file(target));
    // Nothing on stdout, so the report cannot be mistaken for the document.
    CHECK(outcome.out.empty());
    CHECK(outcome.err.find("wrote") != std::string::npos);
}

TEST_CASE("config validate reports rather than fails", "[cli][config]") {
    // The collecting face: this command promises a report, so an invalid
    // document is its answer and not its failure (docs/error-model.md).
    TempDir dir;

    SECTION("a clean config") {
        const auto entry = dir.write("ok.toml", "hoist_orphans = true\n");
        const auto outcome = nhtest::runCaptured({"config", "validate", "--config", entry});
        CHECK(outcome.code == 0);
        CHECK(outcome.mentions("OK"));
    }

    SECTION("a config the validator rejects") {
        const auto entry =
            dir.write("bad.toml", "[[rules]]\nmatch = \"*\"\nmax_segments_circle = -1\n");
        const auto outcome = nhtest::runCaptured({"config", "validate", "--config", entry});
        CHECK(outcome.mentions("INVALID"));
    }
}

TEST_CASE("config validate reaches Lua too", "[cli][config]") {
    // `validate-config` never did: it went straight to the TOML loader, so the
    // only way to check a script was to flatten it and read the diagnostics on
    // the way past.
    TempDir dir;
    const auto entry = dir.write("scene.lua", "config { hoist_orphans = true }\n");

    const auto outcome = nhtest::runCaptured({"config", "validate", "--config", entry});

    INFO("stderr was: " << outcome.err);
    CHECK(outcome.code == 0);
    CHECK(outcome.mentions("OK"));
}

TEST_CASE("the three old spellings are gone", "[cli][config]") {
    // Removed outright rather than aliased. Every release so far is a
    // pre-release, so nothing had promised these names.
    for (const auto *name : {"validate-config", "config-flatten", "config-lua"}) {
        const auto outcome = nhtest::runCaptured({name, "--config", "whatever.toml"});
        INFO("subcommand: " << name);
        CHECK(outcome.code != 0);
    }
}
