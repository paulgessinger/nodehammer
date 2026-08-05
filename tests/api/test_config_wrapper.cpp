// §11's rule, as a test: `Config::read` / `Config::parse` carry no loading
// logic of their own. They delegate to `ConfigLoader` and add only extension
// dispatch, validation and the scene()/output() slicing.
//
// The check is the whole-document one step 5b used: serialise both the wrapped
// and the directly-loaded AST with `configToToml` and compare. A wrapper that
// dropped a field, reordered an include, or resolved one against a different
// base would show up as a text difference, over every fixture at once.
//
// Reaching through `api::` is deliberate and only possible in-tree: the
// seam is an internal header, so this asserts a property an external consumer
// could not even ask about.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <api/handles.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <config/config_writer.hpp>

#include <nodehammer/config.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef NODEHAMMER_FIXTURES_DIR
#error "NODEHAMMER_FIXTURES_DIR must be defined by CMake"
#endif

namespace {

namespace fs = std::filesystem;
namespace api = nodehammer::api;

const fs::path kConfigsDir = fs::path{NODEHAMMER_FIXTURES_DIR} / "configs";

/// Every top-level fixture, in sorted order. Not a hand-maintained list: a new
/// fixture is covered the moment it lands.
std::vector<fs::path> topLevelConfigs() {
    std::vector<fs::path> out;
    for (const auto &entry : fs::directory_iterator{kConfigsDir}) {
        if (entry.is_regular_file() && entry.path().extension() == ".toml") {
            out.push_back(entry.path());
        }
    }
    std::ranges::sort(out);
    return out;
}

std::string readText(const fs::path &path) {
    std::ifstream in{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

bool listHasErrors(const nodehammer::DiagnosticList &diags) { return diags.hasErrors(); }

} // namespace

TEST_CASE("Config::read wraps ConfigLoader without altering the document", "[api][config]") {
    const auto configs = topLevelConfigs();
    REQUIRE_FALSE(configs.empty());

    const auto &path = GENERATE_REF(from_range(configs));
    CAPTURE(path.string());

    // The collecting face is the reference: this test walks every fixture,
    // including the ones that do not parse, and needs the report rather than
    // the throw to decide what `Config::read` should have done.
    const auto reference = nodehammer::config::ConfigLoader::collectFromFile(path);

    // A document that will not load has no half-built form worth returning, so
    // the wrapper throws rather than handing back an empty `Config` nobody can
    // tell apart from a valid one.
    if (reference.diags.hasErrors()) {
        REQUIRE_THROWS_AS(nodehammer::Config::read(path), nodehammer::Error);
        return;
    }
    // Same for a document that loads but does not validate.
    const auto validation = nodehammer::config::ConfigValidator::validate(reference.config);
    if (validation.hasErrors()) {
        REQUIRE_THROWS_AS(nodehammer::Config::read(path), nodehammer::Error);
        return;
    }

    const auto wrapped = nodehammer::Config::read(path);

    REQUIRE(wrapped.config.valid());
    const auto &parsed = wrapped.config.impl().cfg;
    REQUIRE(nodehammer::config::configToToml(parsed) ==
            nodehammer::config::configToToml(reference.config));

    // Validation is the one thing the wrapper adds on top of the load. Its
    // warnings ride back with the config; its errors threw, above.
    REQUIRE_FALSE(listHasErrors(wrapped.diags));
}

TEST_CASE("Config::parse wraps loadFromString and roots includes at baseDir", "[api][config]") {
    // include_nested.toml pulls in a fragment which pulls in another, so a
    // wrapper that resolved against the wrong directory — or not at all — would
    // come back with a different document rather than merely a warning.
    const auto path = kConfigsDir / "include_nested.toml";
    const auto text = readText(path);
    REQUIRE_FALSE(text.empty());

    const auto reference =
        nodehammer::config::ConfigLoader::loadFromString(text, "<string>", kConfigsDir);
    const auto wrapped = nodehammer::Config::parse(text, kConfigsDir);

    REQUIRE_FALSE(reference.diags.hasErrors());
    REQUIRE_FALSE(listHasErrors(wrapped.diags));
    REQUIRE(nodehammer::config::configToToml(wrapped.config.impl().cfg) ==
            nodehammer::config::configToToml(reference.config));

    // And the same content read straight off disk: `parse` + baseDir is
    // `read` minus the file read, which is what step 5b unified.
    const auto fromFile = nodehammer::Config::read(path);
    REQUIRE(nodehammer::config::configToToml(wrapped.config.impl().cfg) ==
            nodehammer::config::configToToml(fromFile.config.impl().cfg));
}

TEST_CASE("Config::parse with no baseDir resolves no includes", "[api][config]") {
    // The property step 5b established, restated at the public boundary: an
    // unnamed location is not the working directory. Planting the include
    // target where a working-directory default would find it is what makes the
    // test able to fail.
    const fs::path planted = fs::current_path() / "nh_api_unrooted_include_probe.toml";
    {
        std::ofstream out{planted};
        REQUIRE(out);
        out << "hoist_orphans = true\n";
    }

    REQUIRE_THROWS_AS(
        nodehammer::Config::parse("include = \"nh_api_unrooted_include_probe.toml\"\n"),
        nodehammer::Error);
    fs::remove(planted);
}

TEST_CASE("Config slices share one document and default to the built-in config", "[api][config]") {
    const auto loaded = nodehammer::Config::read(kConfigsDir / "full_example.toml");
    REQUIRE(loaded.config.valid());

    const auto scene = loaded.config.scene();
    const auto output = loaded.config.output();
    REQUIRE(scene.valid());
    REQUIRE(output.valid());
    // Slicing shares rather than copies: both halves must be the same document.
    REQUIRE(&api::documentOf(scene) == &api::documentOf(output));
    REQUIRE(&api::documentOf(scene) == &loaded.config.impl().cfg);

    // A slice outlives the handle it came from — it owns a share of the
    // document, so this is not a dangling read.
    nodehammer::SceneConfig detached = loaded.config.scene();
    {
        auto temporaryHandle = nodehammer::Config::read(kConfigsDir / "full_example.toml");
        detached = temporaryHandle.config.scene();
    }
    REQUIRE(detached.valid());
    REQUIRE_FALSE(api::documentOf(detached).materials.empty());

    // A default-constructed slice is usable and means "no config file".
    const nodehammer::SceneConfig none;
    REQUIRE_FALSE(none.valid());
    const nodehammer::config::NHConfig defaults;
    REQUIRE(nodehammer::config::configToToml(api::documentOf(none)) ==
            nodehammer::config::configToToml(defaults));
}

TEST_CASE("Config::check reports what Config::read throws", "[api][config]") {
    // The pair the error model is built on: one collector, two promises. `read`
    // owes a config and so cannot return a broken one; `check` owes a report,
    // and a broken document is that report's content.
    constexpr std::string_view bad = R"(
[[rules]]
match = "!!! not an expression"
)";
    const auto report = nodehammer::Config::checkString(bad);
    REQUIRE(report.hasErrors());
    REQUIRE_THROWS_AS(nodehammer::Config::parse(bad), nodehammer::Error);

    // No Fatal in a returned list, ever — that severity belongs to the channel
    // this call deliberately did not use.
    for (const auto &d : report) {
        REQUIRE(d.severity != nodehammer::Diagnostic::Severity::Fatal);
    }

    // Validation failures are reported too, not just parse ones: `check` is the
    // whole of what `read` demands.
    const auto undefinedMaterial = nodehammer::Config::checkString(R"(
[[rules]]
material = "nope"
)");
    REQUIRE(undefinedMaterial.hasErrors());

    // A sound document reports nothing to complain about.
    const auto clean = nodehammer::Config::check(kConfigsDir / "full_example.toml");
    REQUIRE_FALSE(clean.hasErrors());

    // And a file that is not there is not a document to report on.
    REQUIRE_THROWS_AS(nodehammer::Config::check(kConfigsDir / "nope.toml"), nodehammer::Error);
}

TEST_CASE("Config::formats reports what this build can read", "[api][config]") {
    const auto formats = nodehammer::Config::formats();
    REQUIRE(std::ranges::find(formats, "toml") != formats.end());
    // No longer conditional: the interpreter is in every build, wasm included,
    // so a caller reading this list gets the same answer everywhere.
    REQUIRE(std::ranges::find(formats, "lua") != formats.end());

    // A `.lua` path that is not there fails as a missing *file*, which is the
    // distinction that made the old build-gated arm worth having: there is no
    // longer a way to be told "this build cannot read that" for a config.
    REQUIRE_THROWS_AS(nodehammer::Config::read(kConfigsDir / "does_not_exist.lua"),
                      nodehammer::Error);
}
