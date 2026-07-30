#include <catch2/catch_test_macros.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <diagnostic_codes.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────

static nodehammer::config::NHConfig configWithAluminum() {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::MaterialDef mat;
    mat.name = "aluminum";
    mat.metallic = 0.1f;
    mat.roughness = 0.4f;
    cfg.materials.push_back(mat);
    return cfg;
}

// ── Valid config ───────────────────────────────────────────────────────────────

TEST_CASE("ConfigValidator: valid config produces no diagnostics", "[config][validator]") {
    auto cfg = configWithAluminum();
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

TEST_CASE("ConfigValidator: empty config produces no diagnostics", "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

// ── Undefined material reference ──────────────────────────────────────────────

TEST_CASE("ConfigValidator: rule referencing undefined material -> Error", "[config][validator]") {
    auto cfg = configWithAluminum();
    nodehammer::config::Rule rule;
    rule.material = "doesNotExist";
    cfg.rules.push_back(rule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.items().size() == 1);
    REQUIRE(diags.items().front().code == nodehammer::codes::kErrUndefinedMaterialRef);
}

TEST_CASE("ConfigValidator: rule referencing defined material -> no error", "[config][validator]") {
    auto cfg = configWithAluminum();
    nodehammer::config::Rule rule;
    rule.material = "aluminum";
    cfg.rules.push_back(rule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
}

// ── Tessellation rule constraints ─────────────────────────────────────────────

TEST_CASE("ConfigValidator: negative max_segments_circle -> Error", "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::Rule rule;
    rule.tessellation = nodehammer::config::Rule::Tessellation{};
    rule.tessellation->maxSegmentsCircle = -1;
    cfg.rules.push_back(rule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.items().front().code == nodehammer::codes::kErrNegativeTolerance);
}

TEST_CASE("ConfigValidator: zero max_segments_circle -> Error", "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::Rule rule;
    rule.tessellation = nodehammer::config::Rule::Tessellation{};
    rule.tessellation->maxSegmentsCircle = 0;
    cfg.rules.push_back(rule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: positive max_segments_circle -> no error", "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::Rule rule;
    rule.tessellation = nodehammer::config::Rule::Tessellation{};
    rule.tessellation->maxSegmentsCircle = 32;

    cfg.rules.push_back(rule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
}

// ── drop_coincident_faces requires merge_descendants ──────────────────────────

TEST_CASE("ConfigValidator: drop_coincident_faces without merge_descendants -> Warning",
          "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::Rule rule;
    rule.tessellation = nodehammer::config::Rule::Tessellation{};
    rule.tessellation->dropCoincidentFaces = true;
    cfg.rules.push_back(rule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(diags.items().size() == 1);
    REQUIRE(diags.items().front().code == nodehammer::codes::kWarnConfigDropWithoutMerge);
}

TEST_CASE("ConfigValidator: drop_coincident_faces with merge_descendants in a DIFFERENT rule -> ok",
          "[config][validator]") {
    // The supported layered pattern: one rule (e.g. from an included fragment)
    // enables merge_descendants; an overlay rule matching the same nodes enables
    // drop_coincident_faces. The two need not live in the same rule.
    nodehammer::config::NHConfig cfg;
    nodehammer::config::Rule mergeRule;
    mergeRule.tessellation = nodehammer::config::Rule::Tessellation{};
    mergeRule.tessellation->mergeDescendants = true;
    cfg.rules.push_back(mergeRule);
    nodehammer::config::Rule dropRule;
    dropRule.tessellation = nodehammer::config::Rule::Tessellation{};
    dropRule.tessellation->dropCoincidentFaces = true;
    cfg.rules.push_back(dropRule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

TEST_CASE("ConfigValidator: drop_coincident_faces with merge_descendants in defaults -> ok",
          "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    cfg.tessellationDefaults.mergeDescendants = true;
    nodehammer::config::Rule dropRule;
    dropRule.tessellation = nodehammer::config::Rule::Tessellation{};
    dropRule.tessellation->dropCoincidentFaces = true;
    cfg.rules.push_back(dropRule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

// ── Multiple errors accumulate ────────────────────────────────────────────────

TEST_CASE("ConfigValidator: multiple violations all reported", "[config][validator]") {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::Rule matRule;
    matRule.material = "ghost";
    cfg.rules.push_back(matRule);
    nodehammer::config::Rule tessRule;
    tessRule.tessellation = nodehammer::config::Rule::Tessellation{};
    tessRule.tessellation->maxSegmentsCircle = -5;

    cfg.rules.push_back(tessRule);
    auto diags = nodehammer::config::ConfigValidator::validate(cfg);
    REQUIRE(diags.items().size() >= 2); // undefined material + bad segments
}

// ── Round-trip via loadFromString ─────────────────────────────────────────────

TEST_CASE("ConfigValidator: undefined material ref via TOML -> validator catches it",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[[rules]]
material = "ghost"
)";
    auto loaded = nodehammer::config::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(loaded.diags.hasErrors()); // parses fine — semantic error caught by validator
    auto diags = nodehammer::config::ConfigValidator::validate(loaded.config);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: negative max_segments_circle via TOML -> validator catches it",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[[rules]]
[rules.tessellation]
max_segments_circle = -1
fallback = "skip"
)";
    auto loaded = nodehammer::config::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(loaded.diags.hasErrors());
    auto diags = nodehammer::config::ConfigValidator::validate(loaded.config);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: defined material ref via TOML -> validator passes",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[materials.aluminum]
metallic = 0.1
roughness = 0.4

[[rules]]
material = "aluminum"
)";
    auto loaded = nodehammer::config::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(loaded.diags.hasErrors());
    auto diags = nodehammer::config::ConfigValidator::validate(loaded.config);
    REQUIRE_FALSE(diags.hasErrors());
}
