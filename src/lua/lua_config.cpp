#include <nodehammer/lua/lua_config.hpp>

#include <nodehammer/config/color_parse.hpp>
#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/config/config_enums.hpp>
#include <nodehammer/config/predicate_parser.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <sol/sol.hpp>

#include <cstdint>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Option-A Lua config front-end: evaluate a script through a global-function
// builder DSL that assembles an NHConfig. The primitives reuse the existing
// predicate parser (parsePredicateExpr) and enum converters, and replicate the
// TOML loader's field mapping exactly so `config-lua` output round-trips
// through ConfigLoader identically to hand-written TOML. See
// docs/config-scripting-lua.md.

namespace nodehammer {

namespace {

// ── sol helpers ──────────────────────────────────────────────────────────────

bool isPresent(const sol::object &o) {
    const sol::type t = o.get_type();
    return t != sol::type::lua_nil && t != sol::type::none;
}

std::optional<std::string> readFileToString(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ── Predicate parsing (string or list-of-strings → OR) ───────────────────────

std::optional<PredicateExpr> parseOnePredicate(const std::string &expr, const std::string &ctx,
                                               DiagnosticList &diags) {
    auto parsed = parsePredicateExpr(expr);
    if (!parsed) {
        diags.error(
            codes::kErrConfigParse,
            std::format("failed to parse predicate expression '{}': {}", expr, parsed.error()),
            ctx);
        return std::nullopt;
    }
    return std::move(parsed).value();
}

// A DSL predicate value is either a single expression string or a sequence of
// expression strings (OR'd together) — byte-identical to the TOML `match` /
// `keep_if` scalar-or-array forms.
std::optional<PredicateExpr> predicateFromLua(const sol::object &val, const std::string &ctx,
                                              DiagnosticList &diags) {
    if (val.get_type() == sol::type::string) {
        return parseOnePredicate(val.as<std::string>(), ctx, diags);
    }
    if (val.get_type() == sol::type::table) {
        const sol::table arr = val.as<sol::table>();
        const std::size_t n = arr.size();
        std::vector<PredicateExpr> operands;
        operands.reserve(n);
        for (std::size_t i = 1; i <= n; ++i) {
            const sol::object e = arr.get<sol::object>(i);
            if (e.get_type() != sol::type::string) {
                diags.error(codes::kErrConfigParse,
                            std::format("{}: predicate list entry {} must be a string", ctx, i),
                            ctx);
                return std::nullopt;
            }
            auto p = parseOnePredicate(e.as<std::string>(), ctx, diags);
            if (!p) {
                return std::nullopt;
            }
            operands.push_back(std::move(*p));
        }
        if (operands.empty()) {
            diags.warn(codes::kWarnConfigEmptyScope,
                       std::format("{}: empty predicate list — entry skipped", ctx), ctx);
            return std::nullopt;
        }
        return combineOr(std::move(operands));
    }
    diags.error(codes::kErrConfigParse,
                std::format("{}: expected a predicate string or list of strings", ctx), ctx);
    return std::nullopt;
}

// ── Color array parsing (hex strings go through the shared parseHexColor) ────

Color colorFromArray(const sol::table &arr, Color base) {
    if (arr.size() >= 3) {
        base.r = arr.get_or(1, base.r);
        base.g = arr.get_or(2, base.g);
        base.b = arr.get_or(3, base.b);
        if (arr.size() >= 4) {
            base.a = arr.get_or(4, base.a);
        }
    }
    return base;
}

// ── Lua table → nlohmann::json (for `extras`) ────────────────────────────────

nlohmann::json jsonFromLua(const sol::object &o) {
    switch (o.get_type()) {
    case sol::type::boolean:
        return o.as<bool>();
    case sol::type::number:
        // Preserve the Lua 5.4 integer/float distinction so extras round-trip
        // like the TOML loader's int/float handling.
        if (o.is<std::int64_t>()) {
            return nlohmann::json(o.as<std::int64_t>());
        }
        return nlohmann::json(o.as<double>());
    case sol::type::string:
        return o.as<std::string>();
    case sol::type::table: {
        const sol::table t = o.as<sol::table>();
        const std::size_t n = t.size();
        std::size_t keyCount = 0;
        for ([[maybe_unused]] const auto &kv : t) {
            ++keyCount;
        }
        // Contiguous 1..n integer keys with no extras → JSON array.
        if (n > 0 && keyCount == n) {
            nlohmann::json arr = nlohmann::json::array();
            for (std::size_t i = 1; i <= n; ++i) {
                arr.push_back(jsonFromLua(t.get<sol::object>(i)));
            }
            return arr;
        }
        nlohmann::json obj = nlohmann::json::object();
        for (const auto &kv : t) {
            const sol::object k = kv.first;
            std::string key;
            if (k.get_type() == sol::type::string) {
                key = k.as<std::string>();
            } else if (k.get_type() == sol::type::number) {
                key = std::to_string(k.as<std::int64_t>());
            } else {
                continue;
            }
            obj[key] = jsonFromLua(kv.second);
        }
        return obj;
    }
    default:
        return nullptr;
    }
}

// ── Tessellation sub-table → Rule::Tessellation ──────────────────────────────

Rule::Tessellation tessFromLua(const sol::table &t, const std::string &ctx, DiagnosticList &diags) {
    Rule::Tessellation tess;
    if (const sol::optional<bool> v = t["skip_geometry"]) {
        tess.skipGeometry = *v;
    }
    if (const sol::optional<bool> v = t["merge_descendants"]) {
        tess.mergeDescendants = *v;
    }
    if (const sol::optional<int> v = t["max_segments_circle"]) {
        tess.maxSegmentsCircle = *v;
    }
    if (const sol::optional<std::string> v = t["fallback"]) {
        if (auto parsed = parseBooleanFallback(*v)) {
            tess.fallback = *parsed;
        } else {
            diags.error(codes::kErrConfigParse,
                        std::format("unknown fallback '{}'; expected skip, bbox, or fail", *v),
                        ctx);
        }
    }
    return tess;
}

// Deep-freeze helper for `use` imports (§7.1 of the design doc). Returns the
// compiled `readonly` Lua function, or an invalid function if setup failed.
sol::protected_function makeReadonly(sol::state &lua) {
    static constexpr std::string_view kSrc = R"LUA(
local function readonly(t)
  if type(t) ~= "table" then return t end
  local backing = {}
  for k, v in pairs(t) do backing[k] = readonly(v) end
  return setmetatable({}, {
    __index = backing,
    __newindex = function(_, k)
      error(("cannot modify read-only import (key %q)"):format(tostring(k)), 2)
    end,
    __pairs = function() return next, backing, nil end,
    __len = function() return #backing end,
    __metatable = false,
  })
end
return readonly
)LUA";
    sol::load_result loaded = lua.load(kSrc, "=[nh:readonly]");
    if (!loaded.valid()) {
        return {};
    }
    sol::protected_function chunk = loaded;
    sol::protected_function_result r = chunk();
    if (!r.valid()) {
        return {};
    }
    return r;
}

} // namespace

ConfigResult evalLuaConfig(std::string_view src, std::string_view sourceName,
                           const std::filesystem::path &baseDir) {
    NHConfig cfg;
    DiagnosticList diags;
    const std::string chunkName{sourceName};

    try {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);

        // Include-resolution state. `dirStack.back()` is the directory of the
        // currently-executing chunk, so nested include()/use() resolve relative
        // to the including file (mirrors the TOML loader's parent-relative keys).
        std::vector<std::filesystem::path> dirStack{baseDir};
        std::set<std::string> includeStack;                    // on-stack cycle guard for include()
        std::unordered_map<std::string, sol::object> useCache; // run-once cache for use()
        std::set<std::string> useStack;                        // in-progress guard for use()

        sol::protected_function readonly = makeReadonly(lua);

        // load + protected-call a chunk; report load/runtime errors as diagnostics.
        auto runChunk = [&](const std::string &code, const std::string &chunk,
                            bool &ok) -> sol::object {
            ok = false;
            sol::load_result loaded = lua.load(code, chunk);
            if (!loaded.valid()) {
                const sol::error err = loaded;
                diags.error(codes::kErrConfigParse, err.what(), chunk);
                return sol::lua_nil;
            }
            sol::protected_function fn = loaded;
            sol::protected_function_result r = fn();
            if (!r.valid()) {
                const sol::error err = r;
                diags.error(codes::kErrConfigParse, err.what(), chunk);
                return sol::lua_nil;
            }
            ok = true;
            return r;
        };

        // ── config { hoist_orphans=, deduplicate_shapes= } ───────────────────
        lua.set_function("config", [&](sol::table t) {
            if (const sol::optional<bool> v = t["hoist_orphans"]) {
                cfg.hoistOrphans = *v;
            }
            if (const sol::optional<bool> v = t["deduplicate_shapes"]) {
                cfg.deduplicateShapes = *v;
            }
        });

        // ── export(name, { … }) ──────────────────────────────────────────────
        lua.set_function("export", [&](const std::string &name, sol::table t) {
            CommonExportConfig common;
            if (const sol::optional<double> v = t["unit_scale"]) {
                common.unitScale = *v;
            }
            if (const sol::optional<bool> v = t["bake_unit_scale"]) {
                common.bakeUnitScale = *v;
            }
            if (name == "obj") {
                ObjExportFormatConfig c;
                c.common = common;
                cfg.exportFormats[name] = c;
            } else if (name == "gltf" || name == "glb") {
                GltfExportFormatConfig c;
                c.common = common;
                if (const sol::optional<bool> v = t["multi_scene"]) {
                    c.multiScene = *v;
                }
                if (const sol::optional<std::string> v = t["scene_name_separator"]) {
                    c.sceneNameSeparator = *v;
                }
                cfg.exportFormats[name] = c;
            } else {
                diags.warn(
                    codes::kWarnConfigUnknownKey,
                    std::format("unknown export format '{}'; expected gltf, glb, or obj", name),
                    "export");
            }
        });

        // ── material(name, { … }) ────────────────────────────────────────────
        lua.set_function("material", [&](const std::string &name, sol::table t) {
            MaterialDef def;
            def.name = name;
            const sol::object bc = t["base_color"];
            if (bc.get_type() == sol::type::table) {
                def.baseColor = colorFromArray(bc.as<sol::table>(), def.baseColor);
            } else if (bc.get_type() == sol::type::string) {
                const std::string hex = bc.as<std::string>();
                if (auto parsed = parseHexColor(hex)) {
                    def.baseColor = *parsed;
                } else {
                    diags.warn(codes::kWarnConfigUnknownKey,
                               std::format("material '{}': invalid hex color '{}'; expected "
                                           "#RRGGBB or #RRGGBBAA",
                                           name, hex),
                               std::format("material.{}", name));
                }
            }
            if (const sol::optional<float> v = t["metallic"]) {
                def.metallic = *v;
            }
            if (const sol::optional<float> v = t["roughness"]) {
                def.roughness = *v;
            }
            if (const sol::optional<bool> v = t["double_sided"]) {
                def.doubleSided = *v;
            }
            if (const sol::object em = t["emissive"]; em.get_type() == sol::type::table) {
                def.emissive = colorFromArray(em.as<sol::table>(), def.emissive);
            }
            if (const sol::optional<std::string> am = t["alpha_mode"]) {
                if (auto parsed = parseAlphaMode(*am)) {
                    def.alphaMode = *parsed;
                } else {
                    diags.warn(codes::kWarnConfigUnknownKey,
                               std::format("material '{}': unknown alpha_mode '{}'; expected "
                                           "opaque, mask, or blend",
                                           name, *am),
                               std::format("material.{}", name));
                }
            }
            if (const sol::optional<float> v = t["alpha_cutoff"]) {
                def.alphaCutoff = *v;
            }
            if (const sol::optional<float> v = t["ior"]) {
                def.ior = *v;
            }
            if (const sol::optional<float> v = t["transmission"]) {
                def.transmission = *v;
            }
            if (const sol::optional<float> v = t["clearcoat"]) {
                def.clearcoat = *v;
            }
            if (const sol::optional<float> v = t["clearcoat_roughness"]) {
                def.clearcoatRoughness = *v;
            }
            if (const sol::optional<float> v = t["anisotropy"]) {
                def.anisotropy = *v;
            }
            if (const sol::optional<float> v = t["anisotropy_rotation"]) {
                def.anisotropyRotation = *v;
            }
            if (const sol::optional<float> v = t["specular"]) {
                def.specularFactor = *v;
            }
            if (const sol::object sc = t["specular_color"]; sc.get_type() == sol::type::table) {
                def.specularColor =
                    colorFromArray(sc.as<sol::table>(), Color{1.0f, 1.0f, 1.0f, 1.0f});
            }
            cfg.materials.push_back(std::move(def));
        });

        // ── keep{…} / drop{…} — string or list-of-strings ────────────────────
        auto addSelection = [&](SelectionAction action, const sol::object &v) {
            if (auto p = predicateFromLua(v, action == SelectionAction::KeepIf ? "keep" : "drop",
                                          diags)) {
                SelectionRule rule;
                rule.action = action;
                rule.predicate = std::move(*p);
                cfg.selection.push_back(std::move(rule));
            }
        };
        lua.set_function("keep", [&](sol::object v) { addSelection(SelectionAction::KeepIf, v); });
        lua.set_function("drop", [&](sol::object v) { addSelection(SelectionAction::DropIf, v); });

        // ── rule{ match=, material=, tessellation=, extras= } ────────────────
        lua.set_function("rule", [&](sol::table t) {
            Rule rule;
            const sol::object m = t["match"];
            if (m.get_type() == sol::type::string || m.get_type() == sol::type::table) {
                if (auto p = predicateFromLua(m, "rule.match", diags)) {
                    rule.match = std::move(*p);
                }
            } else if (isPresent(m)) {
                diags.error(codes::kErrConfigParse,
                            "rule.match: expected a predicate string or list of strings", "rule");
            }
            // Absent match → matches all nodes (rule.match stays nullopt).

            if (const sol::optional<std::string> mat = t["material"]) {
                rule.material = *mat;
            }
            if (const sol::object te = t["tessellation"]; te.get_type() == sol::type::table) {
                rule.tessellation = tessFromLua(te.as<sol::table>(), "rule.tessellation", diags);
            }
            if (const sol::object ex = t["extras"]; ex.get_type() == sol::type::table) {
                rule.extras = jsonFromLua(ex);
            }
            cfg.rules.push_back(std::move(rule));
        });

        // ── defaults{ tessellation=, extras= } ───────────────────────────────
        lua.set_function("defaults", [&](sol::table t) {
            if (const sol::object te = t["tessellation"]; te.get_type() == sol::type::table) {
                cfg.tessellationDefaults =
                    tessFromLua(te.as<sol::table>(), "defaults.tessellation", diags);
            }
            if (const sol::object ex = t["extras"]; ex.get_type() == sol::type::table) {
                cfg.extrasDefaults = jsonFromLua(ex);
            }
        });

        // ── include(path) — run a fragment into the shared cfg ───────────────
        lua.set_function("include", [&](const std::string &rel) {
            const std::filesystem::path abs =
                std::filesystem::weakly_canonical(dirStack.back() / rel);
            const std::string key = abs.generic_string();
            if (includeStack.contains(key)) {
                diags.error(codes::kErrConfigParse,
                            std::format("include cycle detected: '{}'", rel), key);
                return;
            }
            auto contents = readFileToString(abs);
            if (!contents) {
                diags.error(codes::kErrImportFileNotFound,
                            std::format("include target not found: '{}'", rel), key);
                return;
            }
            includeStack.insert(key);
            dirStack.push_back(abs.parent_path());
            bool ok = false;
            runChunk(*contents, key, ok);
            dirStack.pop_back();
            includeStack.erase(key);
        });

        // ── use(path) — import a library value, cached + deep-frozen ──────────
        lua.set_function("use", [&](const std::string &rel) -> sol::object {
            const std::filesystem::path abs =
                std::filesystem::weakly_canonical(dirStack.back() / rel);
            const std::string key = abs.generic_string();
            if (const auto it = useCache.find(key); it != useCache.end()) {
                return it->second;
            }
            if (useStack.contains(key)) {
                diags.error(codes::kErrConfigParse, std::format("use cycle detected: '{}'", rel),
                            key);
                return sol::lua_nil;
            }
            auto contents = readFileToString(abs);
            if (!contents) {
                diags.error(codes::kErrImportFileNotFound,
                            std::format("use target not found: '{}'", rel), key);
                return sol::lua_nil;
            }
            useStack.insert(key);
            dirStack.push_back(abs.parent_path());
            bool ok = false;
            sol::object mod = runChunk(*contents, key, ok);
            dirStack.pop_back();
            useStack.erase(key);
            if (!ok) {
                return sol::lua_nil;
            }
            sol::object frozen = mod;
            if (mod.get_type() == sol::type::table && readonly.valid()) {
                if (sol::protected_function_result fr = readonly(mod); fr.valid()) {
                    frozen = fr;
                }
            }
            useCache[key] = frozen;
            return frozen;
        });

        // ── run the entry script ─────────────────────────────────────────────
        bool ok = false;
        runChunk(std::string{src}, chunkName, ok);
    } catch (const std::exception &e) {
        diags.error(codes::kErrConfigParse, std::format("internal Lua error: {}", e.what()),
                    chunkName);
    }

    return ConfigResult{std::move(cfg), std::move(diags)};
}

} // namespace nodehammer
