// `Config` and its two slices, through the shared library.
//
// The case that matters most here is the dual channel: `read` and `check` run
// the same collector over the same document and differ only in what they
// promised, so the same TOML has to throw from one and be the ordinary answer
// from the other (docs/error-model.md). Nothing in-tree can check that against
// the *installed* surface, because in-tree code can reach the loader directly.

#include "public_fixture.hpp"

#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace nh = nodehammer;

namespace {

/// Parses and validates. Nothing exotic — dedup is a top-level flag and the
/// export table is the smallest thing that makes `output()` provenance true.
constexpr std::string_view kSound = R"(
deduplicate_shapes = true

[export.gltf]
unit_scale = 0.1
)";

/// Parses, then fails validation: the rule names a material no table defines.
/// Chosen over broken TOML because it reaches the validator as well as the
/// parser, and because its code (NH0002) is unambiguous.
constexpr std::string_view kUndefinedMaterial = R"(
[[rules]]
material = "doesNotExist"
)";

} // namespace

TEST_CASE("Config::parse accepts a sound document", "[public][config]") {
    const auto result = nh::Config::parse(kSound);
    REQUIRE(result.config.valid());
    REQUIRE_FALSE(result.diags.hasErrors());

    // Both slices report provenance, not usability: they came from a real
    // document, so both are true.
    REQUIRE(result.config.scene().valid());
    REQUIRE(result.config.output().valid());
}

TEST_CASE("Config::parse reports an unknown key without failing", "[public][config]") {
    const auto result = nh::Config::parse("[tessellation_rulesx]\nmax_segments_circle = 32\n");
    REQUIRE(result.config.valid());
    REQUIRE_FALSE(result.diags.hasErrors()); // a warning, not a failure

    bool warned = false;
    for (const auto &d : result.diags) {
        if (d.severity == nh::Diagnostic::Severity::Warning && d.code == "NH0005") {
            warned = true;
        }
    }
    REQUIRE(warned);
}

TEST_CASE("Config::read loads a file", "[public][config]") {
    const nhtest::TempDir dir{"config_read"};
    const auto path = dir.put("config.toml", kSound);

    const auto result = nh::Config::read(path);
    REQUIRE(result.config.valid());
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.scene().valid());
}

TEST_CASE("Config::read throws on a document that does not validate", "[public][config]") {
    const nhtest::TempDir dir{"config_invalid"};
    const auto path = dir.put("config.toml", kUndefinedMaterial);

    bool caught = false;
    try {
        (void)nh::Config::read(path);
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0002");
        // The failure does not destroy what led up to it: the collector's whole
        // list rides along, including the error this exception reports.
        REQUIRE(nh::hasErrors(e.observed()));
    }
    REQUIRE(caught);
}

TEST_CASE("Config::check reports what Config::read throws", "[public][config]") {
    // One document, two promises. This is the dual channel stated as a test:
    // the codes have to match, because it is the same collector behind both.
    const nhtest::TempDir dir{"config_check"};
    const auto path = dir.put("config.toml", kUndefinedMaterial);

    std::string thrownCode;
    try {
        (void)nh::Config::read(path);
    } catch (const nh::Error &e) {
        thrownCode = e.code();
    }
    REQUIRE(thrownCode == "NH0002");

    const nh::DiagnosticList reported = nh::Config::check(path);
    REQUIRE(reported.hasErrors());

    bool matched = false;
    for (const auto &d : reported) {
        if (d.code == thrownCode && d.severity == nh::Diagnostic::Severity::Error) {
            matched = true;
        }
    }
    REQUIRE(matched);
}

TEST_CASE("Config::check says nothing about a sound document", "[public][config]") {
    const nhtest::TempDir dir{"config_sound"};
    REQUIRE(nh::Config::check(dir.put("config.toml", "deduplicate_shapes = true\n")).empty());
}

TEST_CASE("Config::check still throws when there is no document", "[public][config]") {
    // It promised a report *about a document*. A file that will not open says
    // nothing about contents, so it stays fatal.
    bool caught = false;
    try {
        (void)nh::Config::check("/nodehammer/definitely/not/here.toml");
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE_FALSE(e.code().empty());
    }
    REQUIRE(caught);
}

TEST_CASE("Config::checkString is the in-memory face", "[public][config]") {
    REQUIRE(nh::Config::checkString("deduplicate_shapes = true\n").empty());

    const auto reported = nh::Config::checkString(kUndefinedMaterial);
    REQUIRE(reported.hasErrors());
    bool named = false;
    for (const auto &d : reported) {
        if (d.code == "NH0002") {
            named = true;
        }
    }
    REQUIRE(named);

    // An empty document is a sound one: every field has a default.
    REQUIRE(nh::Config::checkString("").empty());
}

TEST_CASE("Config::formats reports what this build understands", "[public][config]") {
    const auto formats = nh::Config::formats();
    REQUIRE_FALSE(formats.empty());
    REQUIRE(nhtest::listed(formats, "toml"));

    // A view over library-lifetime storage, so a second call sees the same
    // bytes rather than a fresh container built for the caller.
    REQUIRE(nh::Config::formats().data() == formats.data());
}

TEST_CASE("an empty Config slices to empty slices", "[public][config]") {
    const nh::Config empty;
    REQUIRE_FALSE(empty.valid());

    // Slicing nothing yields an empty slice rather than throwing: a slice is
    // usable either way — it resolves to the built-in defaults — so there is
    // exactly one way to mean "no document" rather than two.
    REQUIRE_FALSE(empty.scene().valid());
    REQUIRE_FALSE(empty.output().valid());
}

TEST_CASE("default-constructed slices mean 'the built-in defaults'", "[public][config]") {
    // Usable, unlike an empty `Config` — which is why `valid()` on a slice
    // reports provenance rather than usability. The verbs accept these, and
    // test_public_build.cpp runs the whole pipeline through one.
    const nh::SceneConfig scene;
    const nh::OutputConfig output;
    REQUIRE_FALSE(scene.valid());
    REQUIRE_FALSE(output.valid());
}
