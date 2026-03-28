#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/config_loader.hpp>

#ifndef NODEHAMMER_FIXTURES_DIR
#error "NODEHAMMER_FIXTURES_DIR must be defined by CMake"
#endif

static std::filesystem::path fixturesDir{NODEHAMMER_FIXTURES_DIR};

// ── loadFromString: basic API ──────────────────────────────────────────────────

TEST_CASE("ConfigLoader: loadFromString succeeds on empty config", "[config][loader]") {
    auto result = nodehammer::ConfigLoader::loadFromString("# empty\n");
    REQUIRE(result.has_value());
    REQUIRE(result->materials.empty());
    REQUIRE(result->selection.empty());
}

TEST_CASE("ConfigLoader: invalid TOML syntax → Error diagnostic with source position",
          "[config][loader]") {
    constexpr std::string_view bad = "[unclosed\nkey = 1\n";
    auto result = nodehammer::ConfigLoader::loadFromString(bad, "test_input");
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(result.error().empty());
    const auto &d = result.error().items().front();
    REQUIRE(d.severity == nodehammer::DiagnosticSeverity::Error);
    REQUIRE(d.context.find("test_input") != std::string::npos);
}

TEST_CASE("ConfigLoader: NHConfig is constructible from C++ without TOML", "[config][loader]") {
    nodehammer::NHConfig cfg;
    nodehammer::MaterialDef mat;
    mat.name = "steel";
    mat.metallic = 0.8f;
    mat.roughness = 0.2f;
    cfg.materials.push_back(mat);
    REQUIRE(cfg.materials.size() == 1);
    REQUIRE(cfg.materials.front().name == "steel");
}

// ── loadFromFile: fixture files ───────────────────────────────────────────────

TEST_CASE("ConfigLoader: minimal.toml loads without diagnostics", "[config][loader][fixtures]") {
    auto result = nodehammer::ConfigLoader::loadFromFile(fixturesDir / "configs/minimal.toml");
    REQUIRE(result.has_value());
    REQUIRE(result->materials.empty());
    REQUIRE(result->selection.empty());
    REQUIRE(result->materialRules.empty());
    REQUIRE(result->tessellationRules.empty());
}

TEST_CASE("ConfigLoader: full_example.toml parses all rule types", "[config][loader][fixtures]") {
    auto result = nodehammer::ConfigLoader::loadFromFile(fixturesDir / "configs/full_example.toml");
    REQUIRE(result.has_value());
    const auto &cfg = *result;

    // Materials — named subtables, key is the name
    REQUIRE(cfg.materials.size() == 2);
    // toml++ iterates in insertion order; find by name
    bool hasAluminum = false;
    bool hasCopper = false;
    for (const auto &m : cfg.materials) {
        if (m.name == "aluminum") {
            hasAluminum = true;
        }
        if (m.name == "copper") {
            hasCopper = true;
        }
    }
    REQUIRE(hasAluminum);
    REQUIRE(hasCopper);

    // Selection rules
    REQUIRE(cfg.selection.size() == 3);
    REQUIRE(cfg.selection.at(0).action == nodehammer::SelectionAction::KeepIf);
    REQUIRE(cfg.selection.at(0).closure == nodehammer::ClosurePolicy::Descendants);
    REQUIRE(cfg.selection.at(0).scope == "/World/Tracker/**");
    REQUIRE(
        std::holds_alternative<nodehammer::NameGlobPredicate>(cfg.selection.at(0).predicate.data));

    REQUIRE(cfg.selection.at(1).action == nodehammer::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::TagPredicate>(cfg.selection.at(1).predicate.data));

    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::AndPredicate>>(
        cfg.selection.at(2).predicate.data));

    // Material rules
    REQUIRE(cfg.materialRules.size() == 2);
    REQUIRE(cfg.materialRules.at(0).materialName == "aluminum");
    REQUIRE(cfg.materialRules.at(0).scope == "/World/Tracker/**");
    REQUIRE(cfg.materialRules.at(0).match.has_value());
    REQUIRE(cfg.materialRules.at(1).materialName == "copper");
    REQUIRE_FALSE(cfg.materialRules.at(1).match.has_value()); // no match = apply to entire scope

    // Tessellation rules
    REQUIRE(cfg.tessellationRules.size() == 2);
    REQUIRE(cfg.tessellationRules.at(0).maxSegmentsCircle == 32);
    REQUIRE(cfg.tessellationRules.at(0).scope == "/World/Tracker/**");
    REQUIRE(cfg.tessellationRules.at(1).maxSegmentsCircle == 64);
    REQUIRE_FALSE(cfg.tessellationRules.at(1).scope.has_value());
}

TEST_CASE("ConfigLoader: missing file → Error diagnostic", "[config][loader]") {
    auto result = nodehammer::ConfigLoader::loadFromFile("/nonexistent/path/config.toml");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().hasErrors());
}

// ── Predicate parsing ─────────────────────────────────────────────────────────

TEST_CASE("ConfigLoader: keep_if with path_glob parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.keep_if]
type = "path_glob"
pattern = "/world/Tracker/**"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(result->selection.size() == 1);
    REQUIRE(result->selection.at(0).action == nodehammer::SelectionAction::KeepIf);
    const auto &pred = result->selection.at(0).predicate;
    REQUIRE(std::holds_alternative<nodehammer::PathGlobPredicate>(pred.data));
    REQUIRE(std::get<nodehammer::PathGlobPredicate>(pred.data).pattern == "/world/Tracker/**");
}

TEST_CASE("ConfigLoader: drop_if with is_leaf parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.drop_if]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(result->selection.at(0).action == nodehammer::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::IsLeafPredicate>(
        result->selection.at(0).predicate.data));
}

TEST_CASE("ConfigLoader: not predicate parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.keep_if]
type = "not"
[selection_rules.keep_if.operand]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::NotPredicate>>(
        result->selection.at(0).predicate.data));
}

TEST_CASE("ConfigLoader: unknown predicate type → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.keep_if]
type = "not_a_real_type"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().hasErrors());
}

TEST_CASE("ConfigLoader: unknown closure value → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
closure = "sideways"
[selection_rules.keep_if]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().hasErrors());
}

TEST_CASE("ConfigLoader: rule missing keep_if or drop_if → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
closure = "none"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ConfigLoader: material rule apply.material is required → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[material_rules]]
scope = "/World/**"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("ConfigLoader: unknown fallback → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[tessellation_rules]]
max_segments_circle = 32
fallback = "explode"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
}
