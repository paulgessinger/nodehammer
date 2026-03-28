#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/config_loader.hpp>

#ifndef NODEHAMMER_FIXTURES_DIR
#error "NODEHAMMER_FIXTURES_DIR must be defined by CMake"
#endif

static std::filesystem::path fixturesDir{NODEHAMMER_FIXTURES_DIR};

// ── loadFromString: basic API ──────────────────────────────────────────────────

TEST_CASE("ConfigLoader: loadFromString succeeds on minimal inline TOML", "[config][loader]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(result->exportCfg.outputPath == "out.glb");
    REQUIRE(result->exportCfg.format == "glb");
}

TEST_CASE("ConfigLoader: invalid TOML syntax → Error diagnostic with source position",
          "[config][loader]") {
    constexpr std::string_view bad = R"(
[output
path = "out.glb"
)"; // missing closing bracket
    auto result = nodehammer::ConfigLoader::loadFromString(bad, "test_input");
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(result.error().empty());
    const auto &d = result.error().items().front();
    REQUIRE(d.severity == nodehammer::DiagnosticSeverity::Error);
    // context should contain source name and line/column
    REQUIRE(d.context.find("test_input") != std::string::npos);
}

TEST_CASE("ConfigLoader: NHConfig is constructible from C++ without TOML", "[config][loader]") {
    nodehammer::NHConfig cfg;
    cfg.exportCfg.outputPath = "out.obj";
    cfg.exportCfg.format = "obj";
    cfg.materials.push_back({"steel", glm::vec4{0.5f, 0.5f, 0.5f, 1.0f}, 0.8f, 0.2f});
    REQUIRE(cfg.materials.size() == 1);
    REQUIRE(cfg.materials.front().name == "steel");
    REQUIRE(cfg.exportCfg.format == "obj");
}

// ── loadFromFile: fixture files ───────────────────────────────────────────────

TEST_CASE("ConfigLoader: minimal.toml loads without diagnostics", "[config][loader][fixtures]") {
    auto result = nodehammer::ConfigLoader::loadFromFile(fixturesDir / "configs/minimal.toml");
    REQUIRE(result.has_value());
    const auto &cfg = *result;
    REQUIRE(cfg.exportCfg.outputPath == "out.glb");
    REQUIRE(cfg.exportCfg.format == "glb");
    REQUIRE(cfg.materials.empty());
    REQUIRE(cfg.selection.empty());
    REQUIRE(cfg.materialRules.empty());
    REQUIRE(cfg.tessellationRules.empty());
}

TEST_CASE("ConfigLoader: full_example.toml parses all rule types", "[config][loader][fixtures]") {
    auto result = nodehammer::ConfigLoader::loadFromFile(fixturesDir / "configs/full_example.toml");
    REQUIRE(result.has_value());
    const auto &cfg = *result;

    // Output
    REQUIRE(cfg.exportCfg.outputPath == "out.glb");
    REQUIRE(cfg.exportCfg.embedExtras == true);

    // Materials
    REQUIRE(cfg.materials.size() == 2);
    REQUIRE(cfg.materials.at(0).name == "aluminum");
    REQUIRE(cfg.materials.at(1).name == "copper");

    // Selection rules
    REQUIRE(cfg.selection.size() == 3);
    REQUIRE(cfg.selection.at(0).action == nodehammer::SelectionAction::KeepIf);
    REQUIRE(cfg.selection.at(0).closure == nodehammer::ClosurePolicy::Descendants);
    REQUIRE(
        std::holds_alternative<nodehammer::NameGlobPredicate>(cfg.selection.at(0).predicate.data));
    REQUIRE(std::get<nodehammer::NameGlobPredicate>(cfg.selection.at(0).predicate.data).pattern ==
            "*Tracker*");

    REQUIRE(cfg.selection.at(1).action == nodehammer::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::TagPredicate>(cfg.selection.at(1).predicate.data));

    // 3rd rule is an "and" compound predicate
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::AndPredicate>>(
        cfg.selection.at(2).predicate.data));

    // Material rules
    REQUIRE(cfg.materialRules.size() == 2);
    REQUIRE(cfg.materialRules.at(0).materialName == "aluminum");
    REQUIRE(cfg.materialRules.at(1).materialName == "copper");

    // Tessellation rules
    REQUIRE(cfg.tessellationRules.size() == 2);
    REQUIRE(cfg.tessellationRules.at(0).maxSegmentsCircle == 32);
    REQUIRE(cfg.tessellationRules.at(0).fallback == nodehammer::BooleanFallback::Skip);
    REQUIRE(cfg.tessellationRules.at(1).maxSegmentsCircle == 64);
}

TEST_CASE("ConfigLoader: missing file → Error diagnostic", "[config][loader]") {
    auto result = nodehammer::ConfigLoader::loadFromFile("/nonexistent/path/config.toml");
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(result.error().empty());
    REQUIRE(result.error().hasErrors());
}

// ── Predicate parsing coverage ────────────────────────────────────────────────

TEST_CASE("ConfigLoader: path_glob predicate parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[selection]]
action = "keep_if"
[selection.predicate]
type = "path_glob"
pattern = "/world/Tracker/**"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(result->selection.size() == 1);
    REQUIRE(std::holds_alternative<nodehammer::PathGlobPredicate>(
        result->selection.at(0).predicate.data));
    REQUIRE(
        std::get<nodehammer::PathGlobPredicate>(result->selection.at(0).predicate.data).pattern ==
        "/world/Tracker/**");
}

TEST_CASE("ConfigLoader: is_leaf predicate parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[selection]]
action = "drop_if"
[selection.predicate]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<nodehammer::IsLeafPredicate>(
        result->selection.at(0).predicate.data));
}

TEST_CASE("ConfigLoader: not predicate parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[selection]]
action = "keep_if"
[selection.predicate]
type = "not"
[selection.predicate.operand]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.has_value());
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::NotPredicate>>(
        result->selection.at(0).predicate.data));
}

TEST_CASE("ConfigLoader: unknown predicate type → Error, parse fails", "[config][loader]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[selection]]
action = "keep_if"
[selection.predicate]
type = "not_a_real_type"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().hasErrors());
}

TEST_CASE("ConfigLoader: unknown selection action → Error, parse fails", "[config][loader]") {
    constexpr std::string_view toml = R"(
[output]
path = "out.glb"
format = "glb"

[[selection]]
action = "do_nothing"
[selection.predicate]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.has_value());
}
