// §11's rule, as a test: `Config::read` / `Config::parse` carry no loading
// logic of their own. They delegate to `ConfigLoader` and add only extension
// dispatch, validation and the scene()/output() slicing.
//
// The check is the whole-document one step 5b used: serialise both the wrapped
// and the directly-loaded AST with `configToToml` and compare. A wrapper that
// dropped a field, reordered an include, or resolved one against a different
// base would show up as a text difference, over every fixture at once.
//
// Reaching through `api::Access` is deliberate and only possible in-tree: the
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
using nodehammer::api::Access;

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

    const auto reference = nodehammer::config::ConfigLoader::loadFromFile(path);
    const auto wrapped = nodehammer::Config::read(path);

    // A load failure has to stay a load failure — and produce no config.
    if (reference.diags.hasErrors()) {
        REQUIRE(listHasErrors(wrapped.diags));
        REQUIRE_FALSE(wrapped.config.valid());
        return;
    }

    REQUIRE(wrapped.config.valid());
    const auto *parsed = Access::configOf(wrapped.config);
    REQUIRE(parsed != nullptr);
    REQUIRE(nodehammer::config::configToToml(*parsed) ==
            nodehammer::config::configToToml(reference.config));

    // Validation is the one thing the wrapper adds on top of the load, so its
    // verdict has to match a direct call rather than being reinvented.
    const auto validation = nodehammer::config::ConfigValidator::validate(reference.config);
    REQUIRE(listHasErrors(wrapped.diags) == validation.hasErrors());
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
    REQUIRE(nodehammer::config::configToToml(*Access::configOf(wrapped.config)) ==
            nodehammer::config::configToToml(reference.config));

    // And the same content read straight off disk: `parse` + baseDir is
    // `read` minus the file read, which is what step 5b unified.
    const auto fromFile = nodehammer::Config::read(path);
    REQUIRE(nodehammer::config::configToToml(*Access::configOf(wrapped.config)) ==
            nodehammer::config::configToToml(*Access::configOf(fromFile.config)));
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

    const auto result =
        nodehammer::Config::parse("include = \"nh_api_unrooted_include_probe.toml\"\n");
    fs::remove(planted);

    REQUIRE(listHasErrors(result.diags));
    REQUIRE_FALSE(result.config.valid());
}

TEST_CASE("Config slices share one document and default to the built-in config", "[api][config]") {
    const auto loaded = nodehammer::Config::read(kConfigsDir / "full_example.toml");
    REQUIRE(loaded.config.valid());

    const auto scene = loaded.config.scene();
    const auto output = loaded.config.output();
    REQUIRE(scene.valid());
    REQUIRE(output.valid());
    // Slicing shares rather than copies: both halves must be the same document.
    REQUIRE(&Access::configOf(scene) == &Access::configOf(output));
    REQUIRE(&Access::configOf(scene) == Access::configOf(loaded.config));

    // A slice outlives the handle it came from — it owns a share of the
    // document, so this is not a dangling read.
    nodehammer::SceneConfig detached = loaded.config.scene();
    {
        auto temporaryHandle = nodehammer::Config::read(kConfigsDir / "full_example.toml");
        detached = temporaryHandle.config.scene();
    }
    REQUIRE(detached.valid());
    REQUIRE_FALSE(Access::configOf(detached).materials.empty());

    // A default-constructed slice is usable and means "no config file".
    const nodehammer::SceneConfig none;
    REQUIRE_FALSE(none.valid());
    const nodehammer::config::NHConfig defaults;
    REQUIRE(nodehammer::config::configToToml(Access::configOf(none)) ==
            nodehammer::config::configToToml(defaults));
}

TEST_CASE("Config::formats reports what this build can read", "[api][config]") {
    const auto formats = nodehammer::Config::formats();
    REQUIRE(std::ranges::find(formats, "toml") != formats.end());
#if NH_WITH_LUA
    REQUIRE(std::ranges::find(formats, "lua") != formats.end());
#else
    REQUIRE(std::ranges::find(formats, "lua") == formats.end());
    // Without the backend, a .lua path is a diagnostic rather than a crash —
    // the format is not known until the extension is looked at (#41 §5).
    const auto result = nodehammer::Config::read(kConfigsDir / "does_not_exist.lua");
    REQUIRE(result.diags.hasErrors());
#endif
}
