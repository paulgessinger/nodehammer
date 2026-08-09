#include <catch2/catch_test_macros.hpp>
#include <config/config_loader.hpp>
#include <config/config_writer.hpp>
#include <diagnostic_codes.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <string>

#ifndef NODEHAMMER_FIXTURES_DIR
#error "NODEHAMMER_FIXTURES_DIR must be defined by CMake"
#endif

static std::filesystem::path fixturesDir{NODEHAMMER_FIXTURES_DIR};

// ── loadFromString: basic API ──────────────────────────────────────────────────

TEST_CASE("ConfigLoader: loadFromString succeeds on empty config", "[config][loader]") {
    auto result = nodehammer::config::ConfigLoader::collectFromString("# empty\n");
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.materials.empty());
    REQUIRE(result.config.selection.empty());
}

TEST_CASE("ConfigLoader: invalid TOML syntax -> Error diagnostic with source position",
          "[config][loader]") {
    constexpr std::string_view bad = "[unclosed\nkey = 1\n";
    auto result = nodehammer::config::ConfigLoader::collectFromString(bad, "test_input");
    REQUIRE(result.diags.hasErrors());
    const auto &d = result.diags.items().front();
    REQUIRE(d.severity == nodehammer::diagnostics::Severity::Error);
    REQUIRE(d.context.find("test_input") != std::string::npos);
}

TEST_CASE("ConfigLoader: NHConfig is constructible from C++ without TOML", "[config][loader]") {
    nodehammer::config::NHConfig cfg;
    nodehammer::config::MaterialDef mat;
    mat.name = "steel";
    mat.metallic = 0.8f;
    mat.roughness = 0.2f;
    cfg.materials.push_back(mat);
    REQUIRE(cfg.materials.size() == 1);
    REQUIRE(cfg.materials.front().name == "steel");
}

// ── loadFromFile: fixture files ───────────────────────────────────────────────

TEST_CASE("ConfigLoader: minimal.toml loads without diagnostics", "[config][loader][fixtures]") {
    auto result =
        nodehammer::config::ConfigLoader::collectFromFile(fixturesDir / "configs/minimal.toml");
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.materials.empty());
    REQUIRE(result.config.selection.empty());
    REQUIRE(result.config.rules.empty());
    REQUIRE(result.diags.empty());
}

TEST_CASE("ConfigLoader: full_example.toml parses all rule types", "[config][loader][fixtures]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/full_example.toml");
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
    REQUIRE(cfg.selection.at(0).action == nodehammer::config::SelectionAction::KeepIf);
    REQUIRE(cfg.selection.at(0).scope == "/World/Tracker/**");
    REQUIRE(std::holds_alternative<nodehammer::config::NameGlobPredicate>(
        cfg.selection.at(0).predicate.data));

    REQUIRE(cfg.selection.at(1).action == nodehammer::config::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::config::TagPredicate>(
        cfg.selection.at(1).predicate.data));

    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::config::AndPredicate>>(
        cfg.selection.at(2).predicate.data));

    // Unified rules (populated from [[rules]])
    REQUIRE(cfg.rules.size() == 4);

    // Rule 0: material + match
    REQUIRE(cfg.rules.at(0).material == "aluminum");
    REQUIRE(cfg.rules.at(0).match.has_value());

    // Rule 1: material + match (path only)
    REQUIRE(cfg.rules.at(1).material == "copper");
    REQUIRE(cfg.rules.at(1).match.has_value());

    // Rule 2: tessellation + extras
    REQUIRE(cfg.rules.at(2).tessellation.has_value());
    REQUIRE(cfg.rules.at(2).tessellation->maxSegmentsCircle == std::optional{32});
    REQUIRE(cfg.rules.at(2).extras.has_value());
    REQUIRE(cfg.rules.at(2).extras->at("visible").get<bool>() == true);

    // Rule 3: fallback tessellation (no match → matches everything)
    REQUIRE(cfg.rules.at(3).tessellation.has_value());
    REQUIRE(cfg.rules.at(3).tessellation->maxSegmentsCircle == std::optional{64});
    REQUIRE_FALSE(cfg.rules.at(3).match.has_value());
}

TEST_CASE("ConfigLoader: a missing file is fatal to both faces", "[config][loader]") {
    // Even to the collecting one: a report promises to describe a document, and
    // there is no document here. An empty report would say the file was fine.
    REQUIRE_THROWS_AS(nodehammer::config::ConfigLoader::collectFromFile("/nonexistent/cfg.toml"),
                      nodehammer::Error);
    REQUIRE_THROWS_AS(nodehammer::config::ConfigLoader::loadFromFile("/nonexistent/cfg.toml"),
                      nodehammer::Error);
}

TEST_CASE("ConfigLoader: the demanding face throws what the collector found", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
match = "!!! not an expression"
)";
    const auto collected = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(collected.diags.hasErrors());

    try {
        (void)nodehammer::config::ConfigLoader::loadFromString(toml);
        FAIL("expected a throw");
    } catch (const nodehammer::Error &e) {
        // Same facts, other channel — and the collection rides along, so a
        // caller that wanted the report can still have it.
        REQUIRE(e.code() == nodehammer::codes::kErrConfigParse);
        REQUIRE(e.observed().size() == collected.diags.size());
        REQUIRE(e.diagnostic().severity == nodehammer::Diagnostic::Severity::Fatal);
    }
}

// ── Predicate parsing ─────────────────────────────────────────────────────────

TEST_CASE("ConfigLoader: keep_if with path_glob parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.keep_if]
type = "path_glob"
pattern = "/world/Tracker/**"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.size() == 1);
    REQUIRE(result.config.selection.at(0).action == nodehammer::config::SelectionAction::KeepIf);
    const auto &pred = result.config.selection.at(0).predicate;
    REQUIRE(std::holds_alternative<nodehammer::config::PathGlobPredicate>(pred.data));
    REQUIRE(std::get<nodehammer::config::PathGlobPredicate>(pred.data).pattern ==
            "/world/Tracker/**");
}

TEST_CASE("ConfigLoader: drop_if with is_leaf parses correctly", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.drop_if]
type = "is_leaf"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.at(0).action == nodehammer::config::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::config::IsLeafPredicate>(
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
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::config::NotPredicate>>(
        result.config.selection.at(0).predicate.data));
}

// ── String expression predicates ─────────────────────────────────────────────

TEST_CASE("ConfigLoader: keep_if as string expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
keep_if = 'path ~= "/world/Tracker/**"'
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.size() == 1);
    REQUIRE(result.config.selection.at(0).action == nodehammer::config::SelectionAction::KeepIf);
    const auto &pred = result.config.selection.at(0).predicate;
    REQUIRE(std::holds_alternative<nodehammer::config::PathGlobPredicate>(pred.data));
    REQUIRE(std::get<nodehammer::config::PathGlobPredicate>(pred.data).pattern ==
            "/world/Tracker/**");
}

TEST_CASE("ConfigLoader: drop_if as string expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
drop_if = 'name ~= "*"'
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.selection.at(0).action == nodehammer::config::SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<nodehammer::config::NameGlobPredicate>(
        result.config.selection.at(0).predicate.data));
}

TEST_CASE("ConfigLoader: keep_if compound expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
keep_if = 'tag.sensitive == "true" && any(path ~= "**/A/**", path ~= "**/B/**")'
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &pred = result.config.selection.at(0).predicate;
    REQUIRE(std::holds_alternative<std::shared_ptr<nodehammer::config::AndPredicate>>(pred.data));
}

TEST_CASE("ConfigLoader: keep_if invalid expression -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
keep_if = 'unknown_keyword'
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: rules match as string expression", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
material = "steel"
match = 'tag.sensitive == "true"'
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.config.rules.at(0).match.has_value());
    REQUIRE(std::holds_alternative<nodehammer::config::TagPredicate>(
        result.config.rules.at(0).match->data));
}

TEST_CASE("ConfigLoader: rules match invalid expression -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
material = "steel"
match = 'bad expression @@'
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: unknown predicate type -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
[selection_rules.keep_if]
type = "not_a_real_type"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: rule missing keep_if or drop_if -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
closure = "none"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: unknown fallback -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
[rules.tessellation]
max_segments_circle = 32
fallback = "explode"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

// ── Unknown-key warnings ──────────────────────────────────────────────────────

TEST_CASE("ConfigLoader: unknown top-level key -> Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[tessellation_rulesx]
max_segments_circle = 32
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors()); // still succeeds
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::diagnostics::Severity::Warning &&
            d.code == nodehammer::codes::kWarnConfigUnknownKey &&
            d.message.find("tessellation_rulesx") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

TEST_CASE("ConfigLoader: unknown key in material table -> Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[materials.steel]
metallick = 0.8
roughness = 0.2
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::diagnostics::Severity::Warning &&
            d.message.find("metallick") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE(result.config.materials.at(0).metallic == 0.0f); // ignored, stays at default
}

TEST_CASE("ConfigLoader: unknown key in rules.tessellation -> Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[rules]]
[rules.tessellation]
xmax_segments_circle = 32
fallback = "skip"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::diagnostics::Severity::Warning &&
            d.message.find("xmax_segments_circle") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE_FALSE(result.config.rules.at(0).tessellation->maxSegmentsCircle.has_value()); // not set
}

TEST_CASE("ConfigLoader: unknown key in selection_rule -> Warning", "[config][loader]") {
    constexpr std::string_view toml = R"(
[[selection_rules]]
scop = "/World/**"
[selection_rules.keep_if]
type = "is_leaf"
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.severity == nodehammer::diagnostics::Severity::Warning &&
            d.message.find("scop") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
    REQUIRE_FALSE(result.config.selection.at(0).scope.has_value()); // ignored
}

// ── Format-specific export key validation ────────────────────────────────────

TEST_CASE("ConfigLoader: gltf-only key under [export.obj] -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
multi_scene = true
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
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

TEST_CASE("ConfigLoader: gltf-only key under [export.gltf] -> no error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.gltf]
multi_scene = true
scene_name_separator = " > "
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &gltfCfg = std::get<nodehammer::config::GltfExportFormatConfig>(
        result.config.exportFormats.at("gltf"));
    REQUIRE(gltfCfg.multiScene == true);
    REQUIRE(gltfCfg.sceneNameSeparator == " > ");
}

TEST_CASE("ConfigLoader: gltf-only key under [export.glb] -> no error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.glb]
multi_scene = true
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &glbCfg =
        std::get<nodehammer::config::GltfExportFormatConfig>(result.config.exportFormats.at("glb"));
    REQUIRE(glbCfg.multiScene == true);
}

// bake_unit_scale is glTF/GLB-only for a stronger reason than the other two:
// ObjExporter rejects a false value outright, so the only legal OBJ value is
// the default. Rejecting the key at load time turns a config that could only
// ever fail at write time into an error the user sees immediately.
TEST_CASE("ConfigLoader: bake_unit_scale under [export.obj] -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
bake_unit_scale = false
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
    bool found = false;
    for (const auto &d : result.diags.items()) {
        if (d.code == nodehammer::codes::kErrConfigParse &&
            d.message.find("bake_unit_scale") != std::string::npos) {
            found = true;
        }
    }
    REQUIRE(found);
}

// Rejected on value `true` as well: the key is not accepted under [export.obj]
// at all, rather than accepted-and-validated. A knob whose only legal value is
// its default is not a knob.
TEST_CASE("ConfigLoader: bake_unit_scale = true under [export.obj] -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
bake_unit_scale = true
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
}

// A misplaced export key is known-but-in-the-wrong-place, so it gets the precise
// error and *not* the generic "unknown key" warning on top of it. Applies to
// every format-specific export key, not just this one.
TEST_CASE("ConfigLoader: a misplaced export key is reported exactly once", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
bake_unit_scale = false
multi_scene = true
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE(result.diags.hasErrors());
    std::size_t unknownKeyWarnings = 0;
    for (const auto &d : result.diags.items()) {
        if (d.message.find("unknown key") != std::string::npos) {
            ++unknownKeyWarnings;
        }
    }
    REQUIRE(unknownKeyWarnings == 0);
    REQUIRE(result.diags.size() == 2); // one error per misplaced key, nothing else
}

TEST_CASE("ConfigLoader: bake_unit_scale under [export.gltf] -> parsed", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.gltf]
bake_unit_scale = true
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &gltfCfg = std::get<nodehammer::config::GltfExportFormatConfig>(
        result.config.exportFormats.at("gltf"));
    REQUIRE(gltfCfg.bakeUnitScale == true);
}

TEST_CASE("ConfigLoader: scene_name_separator under [export.obj] -> Error", "[config][loader]") {
    constexpr std::string_view toml = R"(
[export.obj]
scene_name_separator = " / "
)";
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
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
    auto result = nodehammer::config::ConfigLoader::collectFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE(result.diags.empty());
}

// ── Include mechanism ────────────────────────────────────────────────────────

TEST_CASE("ConfigLoader: basic include merges materials and rules", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/include_basic.toml");
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &cfg = result.config;

    // Materials from included file
    bool hasSteel = false, hasCopper = false;
    for (const auto &m : cfg.materials) {
        if (m.name == "steel") {
            hasSteel = true;
        }
        if (m.name == "copper") {
            hasCopper = true;
        }
    }
    REQUIRE(hasSteel);
    REQUIRE(hasCopper);

    // hoist_orphans from main file
    REQUIRE(cfg.hoistOrphans);

    // Rules from both included and main file:
    // includes/rules.toml has 2 rules, main file has 1 → 3 total.
    REQUIRE(cfg.rules.size() == 3);
}

TEST_CASE("ConfigLoader: nested includes", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/include_nested.toml");
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &cfg = result.config;

    // nested_outer includes nested_inner (aluminum), plus its own copper.
    // Main file adds steel. → 3 materials.
    bool hasAluminum = false, hasCopper = false, hasSteel = false;
    for (const auto &m : cfg.materials) {
        if (m.name == "aluminum") {
            hasAluminum = true;
        }
        if (m.name == "copper") {
            hasCopper = true;
        }
        if (m.name == "steel") {
            hasSteel = true;
        }
    }
    REQUIRE(hasAluminum);
    REQUIRE(hasCopper);
    REQUIRE(hasSteel);
}

TEST_CASE("ConfigLoader: circular include -> Error", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/include_cycle.toml");
    REQUIRE(result.diags.hasErrors());
    bool foundCycle = false;
    for (const auto &d : result.diags.items()) {
        if (d.message.find("circular") != std::string::npos) {
            foundCycle = true;
        }
    }
    REQUIRE(foundCycle);
}

// A diamond is not a cycle. Two siblings including one shared base is the
// normal way to compose configs — it is what fixtures/configs/odd/base.toml
// exists for — and used to be rejected outright, because "already seen
// anywhere" and "already on the current path" were the same set.
TEST_CASE("ConfigLoader: diamond include is not circular", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/include_diamond.toml");
    REQUIRE_FALSE(result.diags.hasErrors());

    // The shared base is merged exactly once: merging it per branch would
    // duplicate its array entries, since arrays concatenate rather than replace.
    std::size_t markerRules = 0;
    for (const auto &r : result.config.rules) {
        if (r.material == "diamond_base_marker") {
            ++markerRules;
        }
    }
    REQUIRE(markerRules == 1);

    // Content from both the base and the root still lands.
    REQUIRE_FALSE(result.config.deduplicateShapes); // from diamond_base.toml
    REQUIRE(result.config.hoistOrphans);            // from the root
}

TEST_CASE("ConfigLoader: include non-existent file -> Error", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(
        fixturesDir / "configs/include_bad_path.toml");
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: parent overrides included scalar", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/include_basic.toml");
    REQUIRE_FALSE(result.diags.hasErrors());
    // hoist_orphans = true in main file; not set in includes → true
    REQUIRE(result.config.hoistOrphans);
}

TEST_CASE("ConfigLoader: included rules come before parent rules", "[config][loader][include]") {
    auto result = nodehammer::config::ConfigLoader::collectFromFile(fixturesDir /
                                                                    "configs/include_basic.toml");
    REQUIRE_FALSE(result.diags.hasErrors());
    const auto &rules = result.config.rules;
    // includes/rules.toml rules come first (match path ~= "**/Tracker/**", then fallback tess),
    // then main file rule (material="copper").
    REQUIRE(rules.size() == 3);
    REQUIRE(rules[0].match.has_value());
    REQUIRE(rules[0].material == "steel");
    REQUIRE(rules[2].material == "copper");
}

// ── Byte-shaped API ──────────────────────────────────────────────────────────

namespace {

std::vector<std::byte> readFileBytes(const std::filesystem::path &path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in);
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

// Build a fetcher that reads from disk relative to a base directory.
// Used to give parseAndMerge equivalent semantics to loadFromFile so the
// equivalence test below is meaningful.
nodehammer::config::IncludeFetcher fsFetcher(const std::filesystem::path &base,
                                             std::vector<std::vector<std::byte>> &owned) {
    return [base, &owned](std::string_view abs_key) -> std::optional<std::span<const std::byte>> {
        auto path = base / std::filesystem::path{abs_key};
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            return std::nullopt;
        }
        in.seekg(0, std::ios::end);
        const auto size = static_cast<std::size_t>(in.tellg());
        in.seekg(0, std::ios::beg);
        std::vector<std::byte> bytes(size);
        in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size));
        owned.push_back(std::move(bytes));
        return std::span<const std::byte>{owned.back()};
    };
}

} // namespace

TEST_CASE("ConfigLoader: peekIncludesFromBytes returns top-level includes",
          "[config][loader][include][bytes]") {
    auto bytes = readFileBytes(fixturesDir / "configs/include_basic.toml");
    auto includes = nodehammer::config::ConfigLoader::peekIncludesFromBytes(bytes);
    REQUIRE(includes.size() == 2);
    REQUIRE(includes[0] == "includes/materials.toml");
    REQUIRE(includes[1] == "includes/rules.toml");
}

TEST_CASE("ConfigLoader: peekIncludesFromBytes returns empty when no include key",
          "[config][loader][include][bytes]") {
    auto bytes = readFileBytes(fixturesDir / "configs/minimal.toml");
    auto includes = nodehammer::config::ConfigLoader::peekIncludesFromBytes(bytes);
    REQUIRE(includes.empty());
}

TEST_CASE("ConfigLoader: resolveIncludeKey computes relative paths against parent",
          "[config][loader][include]") {
    using L = nodehammer::config::ConfigLoader;
    REQUIRE(L::resolveIncludeKey("scene.toml", "common.toml") == "common.toml");
    REQUIRE(L::resolveIncludeKey("scene.toml", "subdir/common.toml") == "subdir/common.toml");
    REQUIRE(L::resolveIncludeKey("subdir/scene.toml", "common.toml") == "subdir/common.toml");
    REQUIRE(L::resolveIncludeKey("subdir/scene.toml", "../common.toml") == "common.toml");
    REQUIRE(L::resolveIncludeKey("a/b/scene.toml", "../c/common.toml") == "a/c/common.toml");
}

TEST_CASE("ConfigLoader: parseAndMerge matches loadFromFile for include_basic",
          "[config][loader][include][bytes]") {
    auto path = fixturesDir / "configs/include_basic.toml";
    auto via_path = nodehammer::config::ConfigLoader::collectFromFile(path);

    auto bytes = readFileBytes(path);
    std::vector<std::vector<std::byte>> owned;
    auto via_bytes = nodehammer::config::ConfigLoader::collectAndMerge(
        bytes, "include_basic.toml", fsFetcher(path.parent_path(), owned));

    REQUIRE_FALSE(via_path.diags.hasErrors());
    REQUIRE_FALSE(via_bytes.diags.hasErrors());
    REQUIRE(via_path.config.hoistOrphans == via_bytes.config.hoistOrphans);
    REQUIRE(via_path.config.materials.size() == via_bytes.config.materials.size());
    REQUIRE(via_path.config.rules.size() == via_bytes.config.rules.size());
}

TEST_CASE("ConfigLoader: parseAndMerge matches loadFromFile for nested includes",
          "[config][loader][include][bytes]") {
    auto path = fixturesDir / "configs/include_nested.toml";
    auto via_path = nodehammer::config::ConfigLoader::collectFromFile(path);

    auto bytes = readFileBytes(path);
    std::vector<std::vector<std::byte>> owned;
    auto via_bytes = nodehammer::config::ConfigLoader::collectAndMerge(
        bytes, "include_nested.toml", fsFetcher(path.parent_path(), owned));

    REQUIRE_FALSE(via_path.diags.hasErrors());
    REQUIRE_FALSE(via_bytes.diags.hasErrors());
    REQUIRE(via_path.config.materials.size() == via_bytes.config.materials.size());
}

TEST_CASE("ConfigLoader: parseAndMerge surfaces missing include as error",
          "[config][loader][include][bytes]") {
    // Synthetic root that includes a non-existent key; fetcher always returns nullopt.
    std::string root_toml = "include = \"missing.toml\"\nhoist_orphans = true\n";
    std::vector<std::byte> bytes(root_toml.size());
    std::memcpy(bytes.data(), root_toml.data(), root_toml.size());

    auto result = nodehammer::config::ConfigLoader::collectAndMerge(
        bytes, "root.toml",
        [](std::string_view) -> std::optional<std::span<const std::byte>> { return std::nullopt; });
    REQUIRE(result.diags.hasErrors());
    bool foundMissing = false;
    for (const auto &d : result.diags.items()) {
        if (d.message.find("not found") != std::string::npos) {
            foundMissing = true;
        }
    }
    REQUIRE(foundMissing);
}

TEST_CASE("ConfigLoader: parseAndMerge detects cycles via key equality",
          "[config][loader][include][bytes]") {
    // Two TOML buffers that include each other.
    std::string a_toml = "include = \"b.toml\"\n";
    std::string b_toml = "include = \"a.toml\"\n";

    std::vector<std::byte> a_bytes(a_toml.size());
    std::memcpy(a_bytes.data(), a_toml.data(), a_toml.size());
    std::vector<std::byte> b_bytes(b_toml.size());
    std::memcpy(b_bytes.data(), b_toml.data(), b_toml.size());

    auto fetcher = [&](std::string_view k) -> std::optional<std::span<const std::byte>> {
        if (k == "a.toml") {
            return std::span<const std::byte>{a_bytes};
        }
        if (k == "b.toml") {
            return std::span<const std::byte>{b_bytes};
        }
        return std::nullopt;
    };

    auto result = nodehammer::config::ConfigLoader::collectAndMerge(a_bytes, "a.toml", fetcher);
    REQUIRE(result.diags.hasErrors());
    bool foundCycle = false;
    for (const auto &d : result.diags.items()) {
        if (d.message.find("circular") != std::string::npos) {
            foundCycle = true;
        }
    }
    REQUIRE(foundCycle);
}

// ── loadFromString rooted at a base directory ────────────────────────────────
//
// The string form resolves includes through the same engine as loadFromFile:
// same fetcher, same merge walk, same cycle rules. These tests are the check
// that the two entry points cannot drift apart.

namespace {

std::string readFileText(const std::filesystem::path &path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE(in);
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Load one fixture both ways and compare the *serialized* configs, so the
// comparison covers the whole document rather than the handful of fields an
// individual include test asserts on.
void requireLoadsIdentically(const std::filesystem::path &path) {
    using nodehammer::config::ConfigLoader;

    auto via_file = ConfigLoader::collectFromFile(path);
    auto via_string = ConfigLoader::collectFromString(readFileText(path), path.filename().string(),
                                                      path.parent_path());

    REQUIRE_FALSE(via_file.diags.hasErrors());
    REQUIRE_FALSE(via_string.diags.hasErrors());
    REQUIRE(nodehammer::config::configToToml(via_string.config) ==
            nodehammer::config::configToToml(via_file.config));
}

} // namespace

TEST_CASE("ConfigLoader: loadFromString with baseDir matches loadFromFile",
          "[config][loader][include]") {
    requireLoadsIdentically(fixturesDir / "configs/include_basic.toml");
    requireLoadsIdentically(fixturesDir / "configs/include_nested.toml");
    requireLoadsIdentically(fixturesDir / "configs/include_diamond.toml");
}

TEST_CASE("ConfigLoader: loadFromString with baseDir reports a circular include",
          "[config][loader][include]") {
    const auto path = fixturesDir / "configs/include_cycle.toml";
    auto result = nodehammer::config::ConfigLoader::collectFromString(
        readFileText(path), path.filename().string(), path.parent_path());

    REQUIRE(result.diags.hasErrors());
    bool foundCycle = false;
    for (const auto &d : result.diags.items()) {
        if (d.message.find("circular") != std::string::npos) {
            foundCycle = true;
        }
    }
    REQUIRE(foundCycle);
}

TEST_CASE("ConfigLoader: loadFromString with baseDir reports a missing include",
          "[config][loader][include]") {
    const auto path = fixturesDir / "configs/include_bad_path.toml";
    auto result = nodehammer::config::ConfigLoader::collectFromString(
        readFileText(path), path.filename().string(), path.parent_path());
    REQUIRE(result.diags.hasErrors());
}

TEST_CASE("ConfigLoader: loadFromString without baseDir never reads the working directory",
          "[config][loader][include]") {
    // The loader invents no base directory: with none given, an include is
    // unresolvable rather than being fetched from wherever the process happens
    // to be. Planting the include target in the working directory is what makes
    // this a real assertion — a loader that silently rooted there would find it
    // and report success.
    const std::filesystem::path planted =
        std::filesystem::current_path() / "nh_unrooted_include_probe.toml";
    {
        std::ofstream out{planted};
        REQUIRE(out);
        out << "hoist_orphans = true\n";
    }

    auto result = nodehammer::config::ConfigLoader::collectFromString(
        "include = \"nh_unrooted_include_probe.toml\"\n");
    std::filesystem::remove(planted);

    REQUIRE(result.diags.hasErrors());
    bool foundMissing = false;
    for (const auto &d : result.diags.items()) {
        if (d.message.find("not found") != std::string::npos) {
            foundMissing = true;
        }
    }
    REQUIRE(foundMissing);
    REQUIRE_FALSE(result.config.hoistOrphans); // the planted file was not merged

    // Naming the directory is what makes it resolvable, and that is the
    // caller's decision to make.
    {
        std::ofstream out{planted};
        REQUIRE(out);
        out << "hoist_orphans = true\n";
    }
    auto rooted = nodehammer::config::ConfigLoader::collectFromString(
        "include = \"nh_unrooted_include_probe.toml\"\n", "<string>",
        std::filesystem::current_path());
    std::filesystem::remove(planted);

    REQUIRE_FALSE(rooted.diags.hasErrors());
    REQUIRE(rooted.config.hoistOrphans);
}

TEST_CASE("ConfigLoader: loadFromString keeps its source name in parse diagnostics",
          "[config][loader]") {
    // The baseDir join must not disturb the context of a plain string load.
    constexpr std::string_view bad = "[unclosed\nkey = 1\n";
    auto result = nodehammer::config::ConfigLoader::collectFromString(bad, "test_input");
    REQUIRE(result.diags.hasErrors());
    REQUIRE(result.diags.items().front().context.starts_with("test_input:"));
}
