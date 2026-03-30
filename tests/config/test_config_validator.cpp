#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────

static nodehammer::NHConfig configWithAluminum() {
    nodehammer::NHConfig cfg;
    nodehammer::MaterialDef mat;
    mat.name = "aluminum";
    mat.metallic = 0.1f;
    mat.roughness = 0.4f;
    cfg.materials.push_back(mat);
    return cfg;
}

// ── Valid config ───────────────────────────────────────────────────────────────

TEST_CASE("ConfigValidator: valid config produces no diagnostics", "[config][validator]") {
    auto cfg = configWithAluminum();
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

TEST_CASE("ConfigValidator: empty config produces no diagnostics", "[config][validator]") {
    nodehammer::NHConfig cfg;
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

// ── Undefined material reference ──────────────────────────────────────────────

TEST_CASE("ConfigValidator: material_rule referencing undefined material → Error",
          "[config][validator]") {
    auto cfg = configWithAluminum();
    nodehammer::MaterialRule rule;
    rule.materialName = "doesNotExist";
    cfg.materialRules.push_back(rule);
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.items().size() == 1);
    REQUIRE(diags.items().front().code == nodehammer::codes::kErrUndefinedMaterialRef);
}

TEST_CASE("ConfigValidator: material_rule referencing defined material → no error",
          "[config][validator]") {
    auto cfg = configWithAluminum();
    nodehammer::MaterialRule rule;
    rule.materialName = "aluminum";
    cfg.materialRules.push_back(rule);
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
}

// ── Tessellation rule constraints ─────────────────────────────────────────────

TEST_CASE("ConfigValidator: negative max_segments_circle → Error", "[config][validator]") {
    nodehammer::NHConfig cfg;
    nodehammer::TessellationRule rule;
    rule.maxSegmentsCircle = -1;
    rule.fallback = nodehammer::BooleanFallback::Skip;
    cfg.tessellationRules.push_back(rule);
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.items().front().code == nodehammer::codes::kErrNegativeTolerance);
}

TEST_CASE("ConfigValidator: zero max_segments_circle → Error", "[config][validator]") {
    nodehammer::NHConfig cfg;
    nodehammer::TessellationRule rule;
    rule.maxSegmentsCircle = 0;
    cfg.tessellationRules.push_back(rule);
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: positive max_segments_circle → no error", "[config][validator]") {
    nodehammer::NHConfig cfg;
    nodehammer::TessellationRule rule;
    rule.maxSegmentsCircle = 32;
    cfg.tessellationRules.push_back(rule);
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
}

// ── Multiple errors accumulate ────────────────────────────────────────────────

TEST_CASE("ConfigValidator: multiple violations all reported", "[config][validator]") {
    nodehammer::NHConfig cfg;
    nodehammer::MaterialRule matRule;
    matRule.materialName = "ghost";
    cfg.materialRules.push_back(matRule);
    nodehammer::TessellationRule tessRule;
    tessRule.maxSegmentsCircle = -5;
    cfg.tessellationRules.push_back(tessRule);
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.items().size() >= 2); // undefined material + bad segments
}

// ── Round-trip via loadFromString ─────────────────────────────────────────────

TEST_CASE("ConfigValidator: undefined material ref via TOML → validator catches it",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[[material_rules]]
apply.material = "ghost"
)";
    auto loaded = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(loaded.diags.hasErrors()); // parses fine — semantic error caught by validator
    auto diags = nodehammer::ConfigValidator::validate(loaded.config);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: negative max_segments_circle via TOML → validator catches it",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[[tessellation_rules]]
max_segments_circle = -1
fallback = "skip"
)";
    auto loaded = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(loaded.diags.hasErrors()); // parses fine — semantic error caught by validator
    auto diags = nodehammer::ConfigValidator::validate(loaded.config);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: defined material ref via TOML → validator passes",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[materials.aluminum]
metallic = 0.1
roughness = 0.4

[[material_rules]]
apply.material = "aluminum"
)";
    auto loaded = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(loaded.diags.hasErrors());
    auto diags = nodehammer::ConfigValidator::validate(loaded.config);
    REQUIRE_FALSE(diags.hasErrors());
}
