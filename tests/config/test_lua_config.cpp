#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_writer.hpp>
#include <nodehammer/lua/lua_config.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

using namespace nodehammer;

namespace {

const std::filesystem::path kLuaFixtures =
    std::filesystem::path{NODEHAMMER_FIXTURES_DIR} / "configs" / "lua";

// Evaluate an inline script with the lua-fixtures dir as the include/use root.
ConfigResult eval(const std::string &src) { return evalLuaConfig(src, "<test>", kLuaFixtures); }

std::optional<std::string> readFile(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool hasUnknownKeyWarning(const DiagnosticList &diags) {
    for (const auto &d : diags.items()) {
        if (d.message.find("unknown key") != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("evalLuaConfig: primitives map onto NHConfig", "[config][lua]") {
    auto r = eval(R"LUA(
config { hoist_orphans = true, deduplicate_shapes = false }
export("gltf", { unit_scale = 0.1, bake_unit_scale = true, multi_scene = true })
material("silicon", { base_color = { 0.1, 0.2, 0.3 }, metallic = 1.0, roughness = 0.25 })
keep { 'path ~= "**/Pixels"' }
drop { 'tag.sensitive == "false"' }
rule {
  match        = 'name ~= "Solenoid*"',
  material     = "silicon",
  tessellation = { max_segments_circle = 48, fallback = "skip" },
  extras       = { visible = true },
}
defaults { tessellation = { max_segments_circle = 10, fallback = "skip" } }
)LUA");

    REQUIRE_FALSE(r.diags.hasErrors());
    const auto &c = r.config;
    REQUIRE(c.hoistOrphans);
    REQUIRE_FALSE(c.deduplicateShapes);
    REQUIRE(c.exportFormats.contains("gltf"));

    REQUIRE(c.materials.size() == 1);
    REQUIRE(c.materials[0].name == "silicon");
    REQUIRE(c.materials[0].metallic == Catch::Approx(1.0f));
    REQUIRE(c.materials[0].baseColor.r == Catch::Approx(0.1f));
    REQUIRE(c.materials[0].baseColor.b == Catch::Approx(0.3f));

    REQUIRE(c.selection.size() == 2);
    REQUIRE(c.selection[0].action == SelectionAction::KeepIf);
    REQUIRE(c.selection[1].action == SelectionAction::DropIf);

    REQUIRE(c.rules.size() == 1);
    REQUIRE(c.rules[0].material == "silicon");
    REQUIRE(c.rules[0].tessellation.has_value());
    REQUIRE(c.rules[0].tessellation->maxSegmentsCircle == 48);
    REQUIRE(c.rules[0].tessellation->fallback == BooleanFallback::Skip);
    REQUIRE(c.rules[0].extras.has_value());

    REQUIRE(c.tessellationDefaults.maxSegmentsCircle == 10);
}

TEST_CASE("evalLuaConfig: hex base_color is sRGB-linearized like the TOML loader",
          "[config][lua]") {
    auto r = eval(R"LUA(material("m", { base_color = "#60666E" }))LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(r.config.materials.size() == 1);
    // #60 = 96/255 = 0.376 sRGB -> ~0.117 linear (matches config_loader hex path).
    REQUIRE(r.config.materials[0].baseColor.r == Catch::Approx(0.1170f).margin(0.001f));
}

TEST_CASE("evalLuaConfig: a predicate list becomes an OR", "[config][lua]") {
    auto r = eval(R"LUA(
rule { match = { 'path ~= "**/A"', 'path ~= "**/B"', 'path ~= "**/C"' } }
)LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(r.config.rules.size() == 1);
    const auto &m = r.config.rules[0].match;
    REQUIRE(m.has_value());
    const auto *orp = std::get_if<std::shared_ptr<OrPredicate>>(&m->data);
    REQUIRE(orp != nullptr);
    REQUIRE((*orp)->operands.size() == 3);
}

TEST_CASE("evalLuaConfig: a single-element list stays a plain predicate", "[config][lua]") {
    auto r = eval(R"LUA(rule { match = { 'path ~= "**/A"' } })LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(r.config.rules.size() == 1);
    const auto &m = r.config.rules[0].match;
    REQUIRE(m.has_value());
    REQUIRE(std::holds_alternative<PathGlobPredicate>(m->data));
}

TEST_CASE("evalLuaConfig: a table-driven loop generates one rule per row", "[config][lua]") {
    auto r = eval(R"LUA(
local names = { "a","b","c","d","e","f","g","h","i","j","k","l","m","n" }
for _, n in ipairs(names) do
  rule { match = ('material ~= "%s"'):format(n) }
end
)LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(r.config.rules.size() == 14);
}

TEST_CASE("evalLuaConfig: use() caches the import instance", "[config][lua]") {
    auto r = eval(R"LUA(
local a = use("lib/constants.lua")
local b = use("lib/constants.lua")
assert(a == b, "use() must return the cached instance")
material("silicon", { base_color = a.palette.silicon })
)LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(r.config.materials.size() == 1);
}

TEST_CASE("evalLuaConfig: use() imports are deep-frozen", "[config][lua]") {
    // Writing through a frozen import raises a Lua error, surfaced as a diagnostic.
    auto r = eval(R"LUA(
local a = use("lib/constants.lua")
a.palette.silicon = "#000000"
)LUA");
    REQUIRE(r.diags.hasErrors());
}

TEST_CASE("evalLuaConfig: syntax errors surface as diagnostics, not crashes", "[config][lua]") {
    auto r = eval("this is not valid lua @@@");
    REQUIRE(r.diags.hasErrors());
}

TEST_CASE("evalLuaConfig: a bad predicate expression is reported", "[config][lua]") {
    auto r = eval(R"LUA(rule { match = 'path ~~ "bad"' })LUA");
    REQUIRE(r.diags.hasErrors());
}

TEST_CASE("evalLuaConfig: include() runs fragments into the shared config", "[config][lua]") {
    auto r = eval(R"LUA(
include("materials.lua")
include("tracker.lua")
)LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    // materials.lua defines 3 materials + 14 map rules; tracker adds keeps/rules.
    REQUIRE(r.config.materials.size() == 3);
    REQUIRE(r.config.rules.size() >= 14);
    REQUIRE_FALSE(r.config.selection.empty());
}

TEST_CASE("evalLuaConfig: result round-trips through configToToml", "[config][lua]") {
    auto r = eval(R"LUA(
material("silicon", { base_color = "#60666E", metallic = 1.0, roughness = 0.05 })
keep { 'path ~= "**/Pixels"', 'path ~= "**/Strips"' }
rule { match = 'path ~= "/world"', tessellation = { skip_geometry = true } }
rule {
  match        = 'name ~= "Sol*"',
  material     = "silicon",
  tessellation = { merge_descendants = true, max_segments_circle = 24, fallback = "bbox" },
}
defaults {
  tessellation = { max_segments_circle = 10, fallback = "skip" },
  extras       = { visible = true },
}
)LUA");
    REQUIRE_FALSE(r.diags.hasErrors());

    const std::string toml = configToToml(r.config);
    CAPTURE(toml);
    auto back = ConfigLoader::loadFromString(toml, "<roundtrip>");
    REQUIRE_FALSE(back.diags.hasErrors());

    REQUIRE(back.config.materials.size() == 1);
    REQUIRE(back.config.selection.size() == 1); // two globs OR'd into one keep rule
    REQUIRE(back.config.rules.size() == 2);
    REQUIRE(back.config.rules[1].tessellation->fallback == BooleanFallback::BBox);
    REQUIRE(back.config.tessellationDefaults.maxSegmentsCircle == 10);
}

// ── Cross-front-end parity (guardrail against key/semantics drift) ───────────
// parity.lua and parity.toml describe the same config exercising every field.
// configToToml is deterministic, so equal serialisations ⟺ equal NHConfig — if
// the two front-ends ever map a key differently, this fails loudly.
TEST_CASE("config front-ends agree: Lua and TOML produce an identical NHConfig", "[config][lua]") {
    const auto luaSrc = readFile(kLuaFixtures / "parity.lua");
    const auto tomlSrc = readFile(kLuaFixtures / "parity.toml");
    REQUIRE(luaSrc.has_value());
    REQUIRE(tomlSrc.has_value());

    auto lua = evalLuaConfig(*luaSrc, "parity.lua", kLuaFixtures);
    auto toml = ConfigLoader::loadFromString(*tomlSrc, "parity.toml");
    REQUIRE_FALSE(lua.diags.hasErrors());
    REQUIRE_FALSE(toml.diags.hasErrors());

    REQUIRE(configToToml(lua.config) == configToToml(toml.config));
}

TEST_CASE("evalLuaConfig: unknown keys are reported like the TOML loader", "[config][lua]") {
    // A typo'd key used to vanish silently on the Lua path; it now warns against
    // the shared per-section allowlist (config_keys.hpp).
    REQUIRE(hasUnknownKeyWarning(
        eval(R"LUA(rule { match = 'path ~= "/w"', mateial = "x" })LUA").diags));
    REQUIRE(hasUnknownKeyWarning(
        eval(R"LUA(material("m", { base_color = "#fff", roughnes = 0.5 }))LUA").diags));
    REQUIRE(
        hasUnknownKeyWarning(eval(R"LUA(rule { tessellation = { max_segments = 8 } })LUA").diags));
    // A clean config produces no unknown-key warning.
    REQUIRE_FALSE(hasUnknownKeyWarning(
        eval(R"LUA(rule { match = 'path ~= "/w"', material = "x" })LUA").diags));
}
