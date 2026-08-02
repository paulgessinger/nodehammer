// The four pipeline verbs and `version()`, through the shared library.

#include "public_fixture.hpp"

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/version.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace nh = nodehammer;

TEST_CASE("version() reports what is linked, not what was compiled against", "[public][version]") {
    // The one entity that proves a caller is talking to libnodehammer rather
    // than just reading its headers: `VERSION` is a constant in the header,
    // `version()` is a symbol in the library, and a mismatch means headers from
    // one install and a library from another.
    const std::string_view linked = nh::version();
    REQUIRE_FALSE(linked.empty());
    REQUIRE(linked == nh::VERSION);

    REQUIRE(nh::VERSION_MAJOR >= 0);
    REQUIRE(std::string{nh::VERSION}.find('.') != std::string::npos);
}

TEST_CASE("the verbs run against a default config", "[public][build]") {
    // A default-constructed slice is usable and means the built-in defaults, so
    // the whole pipeline runs without a document anywhere in sight.
    const auto scene = nhtest::syntheticScene();
    const nh::SceneConfig defaults;

    const auto selected = nh::applySelection(scene, defaults);
    REQUIRE(selected.scene.valid());
    REQUIRE(selected.scene.nodeCount() == scene.nodeCount()); // no rules is a no-op

    const auto deduped = nh::deduplicate(selected.scene, defaults);
    REQUIRE(deduped.scene.valid());

    const auto lowered = nh::tessellate(deduped.scene, defaults);
    REQUIRE(lowered.scene.valid());
    REQUIRE(lowered.scene.triangleCount() > 0);
}

TEST_CASE("build is the three verbs in order", "[public][build]") {
    // #41 §8's claim, from outside: `build` is not a fourth implementation.
    const auto scene = nhtest::syntheticScene();
    const auto config = nh::Config::parse("deduplicate_shapes = true\n");
    const auto sceneConfig = config.config.scene();

    const auto decomposed = nh::tessellate(
        nh::deduplicate(nh::applySelection(scene, sceneConfig).scene, sceneConfig).scene,
        sceneConfig);
    const auto whole = nh::build(scene, sceneConfig);

    REQUIRE(whole.scene.valid());
    REQUIRE(whole.scene.nodeCount() == decomposed.scene.nodeCount());
    REQUIRE(whole.scene.meshCount() == decomposed.scene.meshCount());
    REQUIRE(whole.scene.materialCount() == decomposed.scene.materialCount());
    REQUIRE(whole.scene.triangleCount() == decomposed.scene.triangleCount());
}

TEST_CASE("deduplicate reports only what it observed", "[public][build]") {
    // A one-box scene has nothing to merge, so anything dedup says about it is
    // an observation rather than a complaint. The `Info` channel exists because
    // "dedup ran" and "dedup merged something" are different facts (NH0200);
    // what is checked here is that neither becomes an error.
    const auto result = nh::deduplicate(nhtest::syntheticScene(), nh::SceneConfig{});
    REQUIRE(result.scene.valid());
    REQUIRE_FALSE(result.diags.hasErrors());
    for (const auto &d : result.diags) {
        INFO("[" << d.code << "] " << d.message);
        REQUIRE(d.severity <= nh::Diagnostic::Severity::Warning);
    }
}

TEST_CASE("selection that drops the root is fatal", "[public][build]") {
    // `prune` declines to act, so the scene it would return is the one with the
    // rules *not* applied — not what the verb promised (docs/error-model.md).
    const auto scene = nhtest::syntheticScene();
    const auto config = nh::Config::parse(R"(
[[selection_rules]]
[selection_rules.drop_if]
type = "name_glob"
pattern = "*"
)");
    REQUIRE(config.config.valid());
    const auto sceneConfig = config.config.scene();

    bool caughtFromVerb = false;
    try {
        (void)nh::applySelection(scene, sceneConfig);
    } catch (const nh::Error &e) {
        caughtFromVerb = true;
        REQUIRE(e.code() == "NH0401");
    }
    REQUIRE(caughtFromVerb);

    // `build` runs the same stage, so it throws the same thing rather than
    // deciding for itself.
    bool caughtFromBuild = false;
    try {
        (void)nh::build(scene, sceneConfig);
    } catch (const nh::Error &e) {
        caughtFromBuild = true;
        REQUIRE(e.code() == "NH0401");
    }
    REQUIRE(caughtFromBuild);
}

TEST_CASE("every verb rejects a handle that refers to nothing", "[public][build]") {
    // A caller mistake with no result to attach a reason to. The context names
    // the verb, so the message says which call was wrong rather than only which
    // type.
    const nh::SemanticScene empty;
    const nh::SceneConfig defaults;

    const auto rejects = [&](auto &&verb, std::string_view name) {
        bool caught = false;
        try {
            (void)verb(empty, defaults);
        } catch (const nh::Error &e) {
            caught = true;
            INFO("verb: " << name);
            REQUIRE(e.code() == "NH0800");
            REQUIRE(e.context() == name);
        }
        REQUIRE(caught);
    };

    rejects(nh::applySelection, "applySelection");
    rejects(nh::deduplicate, "deduplicate");
    rejects(nh::tessellate, "tessellate");
    rejects(nh::build, "build");
}
