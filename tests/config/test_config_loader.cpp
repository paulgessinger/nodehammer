#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#ifndef NODEHAMMER_FIXTURES_DIR
#error "NODEHAMMER_FIXTURES_DIR must be defined by CMake"
#endif

static std::filesystem::path fixturesDir{NODEHAMMER_FIXTURES_DIR};

// ── loadFromString: basic API ──────────────────────────────────────────────────

TEST_CASE("ConfigLoader: loadFromString succeeds on empty config", "[config][loader]") {
    auto result = nodehammer::ConfigLoader::loadFromString("# empty\n");
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.materials.empty());
    REQUIRE(result.config.selection.empty());
}

TEST_CASE("ConfigLoader: invalid TOML syntax → Error diagnostic with source position",
          "[config][loader]") {
    constexpr std::string_view bad = "[unclosed\nkey = 1\n";
    auto result = nodehammer::ConfigLoader::loadFromString(bad, "test_input");
    REQUIRE(result.diags.hasErrors());
    const auto &d = result.diags.items().front();
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
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.materials.empty());
    REQUIRE(result.config.selection.empty());
    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diags.empty());
}

TEST_CASE("ConfigLoader: full_example.toml parses all rule types", "[config][loader][fixtures]") {
    auto result = nodehammer::ConfigLoader::loadFromFile(fixturesDir / "configs/full_example.toml");
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.diags.empty()); // clean fixture — no warnings
    const auto &cfg = result.config;

    // Materials — named subtables, key is the name
    REQUIRE(cfg.materials.size() == 2);
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

    // Unified rules (populated from [[rules]])
    REQUIRE(cfg.rules.size() == 4);

    // Rule 0: material + match
    REQUIRE(cfg.rules.at(0).material == "aluminum");
    REQUIRE(cfg.rules.at(0).scope == "/World/Tracker/**");
    REQUIRE(cfg.rules.at(0).match.has_value());

    // Rule 1: material only
    REQUIRE(cfg.rules.at(1).material == "copper");
    REQUIRE_FALSE(cfg.rules.at(1).match.has_value());

    // Rule 2: tessellation + extras
    REQUIRE(cfg.rules.at(2).tessellation.has_value());
    REQUIRE(cfg.rules.at(2).tessellation->maxSegmentsCircle == 32);
    REQUIRE(cfg.rules.at(2).extras.has_value());
    REQUIRE(cfg.rules.at(2).extras->at("visible").get<bool>() == true);

    // Rule 3: fallback tessellation
    REQUIRE(cfg.rules.at(3).tessellation.has_value());
    REQUIRE(cfg.rules.at(3).tessellation->maxSegmentsCircle == 64);
    REQUIRE_FALSE(cfg.rules.at(3).scope.has_value());
}

TEST_CASE("ConfigLoader: missing file → Error diagnostic", "[config][loader]") {
    auto result = nodehammer::ConfigLoader::loadFromFile("/nonexistent/path/config.toml");
    REQUIRE(result.diags.hasErrors());
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
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.size() == 1);
    REQUIRE(result.config.selection.at(0).action == nodehammer::SelectionAction::KeepIf);
    const auto &pred = result.config.selection.at(0).predicate;
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
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.at(0).action == nodehammer::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::IsLeafPredicate>(
        result.config.selection.at(0).predicate.data));
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
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::NotPredicate>>(
        result.config.selection.at(0).predicate.data));
}

// ── String expression predicates ─────────────────────────────────────────────

TEST_CASE("ConfigLoader: keep_if as string expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
keep_if = 'path ~= "/world/Tracker/**"'
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.size() == 1);
    REQUIRE(result.config.selection.at(0).action == nodehammer::SelectionAction::KeepIf);
    const auto &pred = result.config.selection.at(0).predicate;
    REQUIRE(std::holds_alternative<nodehammer::PathGlobPredicate>(pred.data));
    REQUIRE(std::get<nodehammer::PathGlobPredicate>(pred.data).pattern == "/world/Tracker/**");
}

TEST_CASE("ConfigLoader: drop_if as string expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
drop_if = 'name ~= "*"'
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.at(0).action == nodehammer::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::NameGlobPredicate>(
        result.config.selection.at(0).predicate.data));
}

TEST_CASE("ConfigLoader: keep_if compound expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
keep_if = 'tag.sensitive == "true" && any(path ~= "**/A/**", path ~= "**/B/**")'
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &pred = result.config.selection.at(0).predicate;
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::AndPredicate>>(pred.data));
}

TEST_CASE("ConfigLoader: keep_if invalid expression → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
keep_if = 'unknown_keyword'
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: rules match as string expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
material = "steel"
match = 'tag.sensitive == "true"'
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.rules.at(0).match.has_value());
    REQUIRE(
        std::holds_alternative<nodehammer::TagPredicate>(result.config.rules.at(0).match->data));
}

TEST_CASE("ConfigLoader: rules match invalid expression → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
material = "steel"
match = 'bad expression @@'
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: unknown predicate type → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.keep_if]
type = "not_a_real_type"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: unknown closure value → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
closure = "sideways"
[selection_rules.keep_if]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: rule missing keep_if or drop_if → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
closure = "none"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: unknown fallback → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
[rules.tessellation]
max_segments_circle = 32
fallback = "explode"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

// ── Unknown-key warnings ──────────────────────────────────────────────────────

TEST_CASE("ConfigLoader: unknown top-level key → Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[tessellation_rulesx]
max_segments_circle = 32
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors()); // still succeeds
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::DiagnosticSeverity::Warning &&
            d.code == nodehammer::codes::kWarnConfigUnknownKey &&
            d.message.find("tessellation_rulesx") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader: unknown key in material table → Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[materials.steel]
metallick = 0.8
roughness = 0.2
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::DiagnosticSeverity::Warning &&
            d.message.find("metallick") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(result.config.materials.at(0).metallic == 0.0f); // ignored, stays at default
}

TEST_CASE("ConfigLoader: unknown key in rules.tessellation → Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
[rules.tessellation]
xmax_segments_circle = 32
fallback = "skip"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::DiagnosticSeverity::Warning &&
            d.message.find("xmax_segments_circle") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(result.config.rules.at(0).tessellation->maxSegmentsCircle == 64); // default
}

TEST_CASE("ConfigLoader: unknown key in selection_rule → Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
scop = "/World/**"
[selection_rules.keep_if]
type = "is_leaf"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::DiagnosticSeverity::Warning &&
            d.message.find("scop") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE_FALSE(result.config.selection.at(0).scope.has_value()); // ignored
}

// ── Format-specific export key validation ────────────────────────────────────

TEST_CASE("ConfigLoader: gltf-only key under [export.obj] → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
multi_scene = true
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == nodehammer::codes::kErrConfigParse &&
            d.message.find("multi_scene") != std::string::npos &&
            d.message.find("export.obj") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader: gltf-only key under [export.gltf] → no error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.gltf]
multi_scene = true
scene_name_separator = " > "
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &gltfCfg =
        std::get<nodehammer::GltfExportFormatConfig>(result.config.exportFormats.at("gltf"));
    REQUIRE(gltfCfg.multiScene == true);
    REQUIRE(gltfCfg.sceneNameSeparator == " > ");
}

TEST_CASE("ConfigLoader: gltf-only key under [export.glb] → no error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.glb]
multi_scene = true
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &glbCfg =
        std::get<nodehammer::GltfExportFormatConfig>(result.config.exportFormats.at("glb"));
    REQUIRE(glbCfg.multiScene == true);
}

TEST_CASE("ConfigLoader: scene_name_separator under [export.obj] → Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
scene_name_separator = " / "
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == nodehammer::codes::kErrConfigParse &&
            d.message.find("scene_name_separator") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader: clean config produces no warnings", "[config][loader]") {
    constexpr std::string_view toml = R"(
[materials.steel]
metallic = 0.8
roughness = 0.2

[[rules]]
[rules.tessellation]
max_segments_circle = 32
fallback = "skip"
)";
    auto result = nodehammer::ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.diags.empty());
}
