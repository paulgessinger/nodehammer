#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

// ── Helpers ───────────────────────────────────────────────────────────────────

static nodehammer::NHConfig validBaseConfig() {
    nodehammer::NHConfig cfg;
    cfg.exportCfg.outputPath = "out.glb";
    cfg.exportCfg.format = "glb";
    cfg.materials.push_back({"aluminum", glm::vec4{0.75f, 0.75f, 0.85f, 1.0f}, 0.1f, 0.4f});
    return cfg;
}

// ── Valid config ───────────────────────────────────────────────────────────────

TEST_CASE("ConfigValidator: valid config produces no diagnostics", "[config][validator]") {
    auto cfg = validBaseConfig();
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.empty());
}

// ── Undefined material reference ──────────────────────────────────────────────

TEST_CASE("ConfigValidator: material_rule referencing undefined material → Error",
          "[config][validator]") {
    auto cfg = validBaseConfig();
    cfg.materialRules.push_back({"*", "doesNotExist"});
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.items().size() == 1);
    REQUIRE(diags.items().front().code == nodehammer::codes::kErrUndefinedMaterialRef);
}

TEST_CASE("ConfigValidator: material_rule referencing defined material → no error",
          "[config][validator]") {
    auto cfg = validBaseConfig();
    cfg.materialRules.push_back({"*", "aluminum"});
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
}

// ── Tessellation rule constraints ─────────────────────────────────────────────

TEST_CASE("ConfigValidator: negative max_segments_circle → Error", "[config][validator]") {
    auto cfg = validBaseConfig();
    cfg.tessellationRules.push_back({"*", -1, nodehammer::BooleanFallback::Skip});
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.items().front().code == nodehammer::codes::kErrNegativeTolerance);
}

TEST_CASE("ConfigValidator: zero max_segments_circle → Error", "[config][validator]") {
    auto cfg = validBaseConfig();
    cfg.tessellationRules.push_back({"*", 0, nodehammer::BooleanFallback::Skip});
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: positive max_segments_circle → no error", "[config][validator]") {
    auto cfg = validBaseConfig();
    cfg.tessellationRules.push_back({"*", 32, nodehammer::BooleanFallback::Skip});
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE_FALSE(diags.hasErrors());
}

// ── Output path required ──────────────────────────────────────────────────────

TEST_CASE("ConfigValidator: missing output path → Error", "[config][validator]") {
    nodehammer::NHConfig cfg;
    // No outputPath set
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.hasErrors());
    bool hasMissingPath = false;
    for (const auto &d : diags.items()) {
        if (d.code == nodehammer::codes::kErrMissingOutputPath) {
            hasMissingPath = true;
        }
    }
    REQUIRE(hasMissingPath);
}

// ── Multiple errors accumulate ────────────────────────────────────────────────

TEST_CASE("ConfigValidator: multiple violations all reported", "[config][validator]") {
    nodehammer::NHConfig cfg;
    // No output path
    cfg.materialRules.push_back({"*", "ghost"}); // undefined material
    cfg.tessellationRules.push_back({"*", -5, nodehammer::BooleanFallback::Skip}); // bad segments
    auto diags = nodehammer::ConfigValidator::validate(cfg);
    REQUIRE(diags.items().size() >= 3); // missing path + undefined material + bad segments
}

// ── Round-trip via loadFromString ─────────────────────────────────────────────

TEST_CASE("ConfigValidator: invalid_missing_material_ref TOML → validator catches it",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[material_rules]]
name_glob = "*"
material = "ghost"
)";
    auto loaded = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(loaded.has_value()); // parses fine — validator catches the semantic error
    auto diags = nodehammer::ConfigValidator::validate(*loaded);
    REQUIRE(diags.hasErrors());
}

TEST_CASE("ConfigValidator: invalid_bad_tolerance TOML → validator catches it",
          "[config][validator]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[tessellation_rules]]
name_glob = "*"
max_segments_circle = -1
fallback = "skip"
)";
    auto loaded = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(loaded.has_value()); // parses fine — validator catches it
    auto diags = nodehammer::ConfigValidator::validate(*loaded);
    REQUIRE(diags.hasErrors());
}
