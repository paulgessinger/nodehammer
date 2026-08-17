#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <config/config_loader.hpp>
#include <config/config_writer.hpp>
#include <lua/lua_config.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace nodehammer;
using namespace nodehammer::diagnostics;
using namespace nodehammer::lua;
using namespace nodehammer::config;

namespace {

const std::filesystem::path kLuaFixtures =
    std::filesystem::path{NODEHAMMER_FIXTURES_DIR} / "configs" / "lua";

// Evaluate an inline script with the lua-fixtures dir as the include/use root.
//
// The root key is a name *inside* that directory rather than the directory
// itself: `resolveIncludeKey` takes the parent of the key it is given, so a key
// of `<fixtures>/<test>` is what makes `include("materials.lua")` land beside
// the fixtures. This mirrors `ConfigLoader::collectFromString`, which joins its
// baseDir onto the source name for exactly the same reason.
ConfigResult eval(const std::string &src) {
    return evalLuaConfig(src, (kLuaFixtures / "<test>").generic_string(),
                         ConfigLoader::filesystemFetcher());
}

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

    auto lua = evalLuaConfig(*luaSrc, (kLuaFixtures / "parity.lua").generic_string(),
                             ConfigLoader::filesystemFetcher());
    auto toml = ConfigLoader::loadFromString(*tomlSrc, "parity.toml");
    REQUIRE_FALSE(lua.diags.hasErrors());
    REQUIRE_FALSE(toml.diags.hasErrors());

    REQUIRE(configToToml(lua.config) == configToToml(toml.config));
}

TEST_CASE("evalLuaConfig: include()/use() cannot escape the config root", "[config][lua]") {
    // A `..` that climbs above baseDir is rejected before any file read.
    REQUIRE(eval(R"LUA(include("../escape.lua"))LUA").diags.hasErrors());
    REQUIRE(eval(R"LUA(use("../escape.lua"))LUA").diags.hasErrors());
    REQUIRE(eval(R"LUA(include("../../etc/passwd"))LUA").diags.hasErrors());
}

TEST_CASE("evalLuaConfig: invalid hex colors are reported, not silently decoded", "[config][lua]") {
    const auto hasHexWarning = [](const DiagnosticList &diags) {
        for (const auto &d : diags.items()) {
            if (d.message.find("invalid hex color") != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    REQUIRE(hasHexWarning(eval(R"LUA(material("m", { base_color = "#GGGGGG" }))LUA").diags));
    REQUIRE(hasHexWarning(eval(R"LUA(material("m", { base_color = "#1234567" }))LUA").diags));
    REQUIRE_FALSE(hasHexWarning(eval(R"LUA(material("m", { base_color = "#123456" }))LUA").diags));
}

TEST_CASE("evalLuaConfig: a map-like predicate table is rejected", "[config][lua]") {
    REQUIRE(eval(R"LUA(rule { match = { foo = "bar" } })LUA").diags.hasErrors());
}

TEST_CASE("evalLuaConfig: a runaway loop is stopped by the instruction budget", "[config][lua]") {
    // Bounded by the lua_sethook count guard rather than hanging the process.
    REQUIRE(eval("while true do end").diags.hasErrors());
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

// Both front-ends now walk the same keys::kExportKeys table, so a format-specific
// key misplaced in Lua is an *error* with a precise message, exactly as in TOML --
// only the syntax in the message differs.
TEST_CASE("evalLuaConfig: bake_unit_scale is glTF/GLB-only", "[config][lua]") {
    auto gltf = eval(R"LUA(export("gltf", { bake_unit_scale = true }))LUA");
    REQUIRE_FALSE(gltf.diags.hasErrors());
    REQUIRE_FALSE(hasUnknownKeyWarning(gltf.diags));
    REQUIRE(std::get<GltfExportFormatConfig>(gltf.config.exportFormats.at("gltf")).bakeUnitScale ==
            true);

    auto obj = eval(R"LUA(export("obj", { bake_unit_scale = true }))LUA");
    REQUIRE(obj.diags.hasErrors());
    bool precise = false;
    for (const auto &d : obj.diags.items()) {
        if (d.message.find("bake_unit_scale") != std::string::npos &&
            d.message.find("export('gltf')") != std::string::npos) {
            precise = true;
        }
    }
    REQUIRE(precise);
    // Reported once: misplaced, not unknown.
    REQUIRE_FALSE(hasUnknownKeyWarning(obj.diags));
}

// The escalation covers every format-specific key, not just the new one --
// that is the point of both front-ends reading one table.
TEST_CASE("evalLuaConfig: multi_scene misplaced under obj is an error too", "[config][lua]") {
    auto r = eval(R"LUA(export("obj", { multi_scene = true }))LUA");
    REQUIRE(r.diags.hasErrors());
    REQUIRE_FALSE(hasUnknownKeyWarning(r.diags));
}

// A genuinely unknown key is still just a warning on both paths.
TEST_CASE("evalLuaConfig: unknown export key stays a warning", "[config][lua]") {
    auto r = eval(R"LUA(export("obj", { frobnicate = 1 }))LUA");
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(hasUnknownKeyWarning(r.diags));
}

// ── The seam the fetcher exists for ──────────────────────────────────────────
//
// Everything above resolves includes off the real filesystem, because that is
// what a CLI has. A project archive is the case that has no filesystem at all:
// the bytes live in a ZIP working set and reach the loader through a fetcher
// over keys. These cases evaluate a script whose includes exist *only* in a map,
// which is the same shape `BuildSession` serves and the reason `evalLuaConfig`
// takes a fetcher rather than a directory.
namespace {

/// A fetcher over an in-memory key→bytes map, like a project's working set.
struct MemoryFiles {
    std::unordered_map<std::string, std::vector<std::byte>> files;
    std::vector<std::string> requested; // in call order, for the include closure

    void put(std::string key, std::string_view content) {
        const auto *p = reinterpret_cast<const std::byte *>(content.data());
        files.emplace(std::move(key), std::vector<std::byte>{p, p + content.size()});
    }

    IncludeFetcher fetcher() {
        return [this](std::string_view key) -> std::optional<std::span<const std::byte>> {
            requested.emplace_back(key);
            const auto it = files.find(std::string{key});
            if (it == files.end()) {
                return std::nullopt;
            }
            return std::span<const std::byte>{it->second};
        };
    }
};

} // namespace

TEST_CASE("evalLuaConfig: includes resolve through a fetcher with no filesystem", "[config][lua]") {
    MemoryFiles project;
    project.put("proj/materials.lua", R"LUA(
material("silicon", { metallic = 1.0 })
)LUA");
    project.put("proj/lib/palette.lua", R"LUA(
return { kapton = "#806040" }
)LUA");
    project.put("proj/rules.lua", R"LUA(
local P = use("lib/palette.lua")
material("kapton", { base_color = P.kapton })
for i = 1, 3 do
  rule { match = ('path ~= "**/Layer%d*"'):format(i), material = "silicon" }
end
)LUA");

    const std::string root = R"LUA(
config { deduplicate_shapes = true }
include("materials.lua")
include("rules.lua")
)LUA";

    const auto r = evalLuaConfig(root, "proj/config.lua", project.fetcher());
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(r.config.deduplicateShapes);
    REQUIRE(r.config.materials.size() == 2);
    REQUIRE(r.config.rules.size() == 3);

    // `use()` inside an included fragment resolves against *that* fragment's
    // key, not the root's — which is the whole reason the key stack exists.
    REQUIRE(std::ranges::find(project.requested, "proj/lib/palette.lua") !=
            project.requested.end());
}

TEST_CASE("evalLuaConfig: the keys a script asks for are its include closure", "[config][lua]") {
    // What a project archive needs to know in order to pack itself: a Lua
    // config's include set cannot be read off the source the way TOML's
    // `include = [...]` can, because it is computed. Recording what the fetcher
    // was asked for is the answer, and it is exact.
    MemoryFiles project;
    project.put("proj/a.lua", "include(\"b.lua\")\n");
    project.put("proj/b.lua", "material(\"m\", {})\n");

    const auto r = evalLuaConfig("include(\"a.lua\")\n", "proj/root.lua", project.fetcher());
    REQUIRE_FALSE(r.diags.hasErrors());
    REQUIRE(project.requested == std::vector<std::string>{"proj/a.lua", "proj/b.lua"});
}

TEST_CASE("evalLuaConfig: a missing key in a project is reported, not fatal to the process",
          "[config][lua]") {
    MemoryFiles project; // deliberately empty
    const auto r = evalLuaConfig("include(\"gone.lua\")\n", "proj/root.lua", project.fetcher());
    REQUIRE(r.diags.hasErrors());
    REQUIRE(project.requested == std::vector<std::string>{"proj/gone.lua"});
}

TEST_CASE("evalLuaConfig: the root guard does not depend on the fetcher", "[config][lua]") {
    // A fetcher that would happily serve anything still does not get asked:
    // the escape is refused in key space, so an untrusted script's reach is the
    // same whether it is running against a ZIP or against a disk.
    MemoryFiles project;
    project.put("secrets.lua", "material(\"leaked\", {})\n");

    const auto r =
        evalLuaConfig("include(\"../secrets.lua\")\n", "proj/root.lua", project.fetcher());
    REQUIRE(r.diags.hasErrors());
    REQUIRE(r.config.materials.empty());
    REQUIRE(project.requested.empty()); // never even asked
}
