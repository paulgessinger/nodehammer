#include <lua/lua_config.hpp>

#include <config/color_parse.hpp>
#include <config/config_ast.hpp>
#include <config/config_enums.hpp>
#include <config/config_keys.hpp>
#include <config/predicate_parser.hpp>
#include <diagnostic_codes.hpp>

#include <sol/sol.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// sol2's proxy/reference types (table_proxy, load_result, protected_function_result)
// convert to sol::object / sol::optional / sol::protected_function through
// user-defined conversion operators. GCC/Clang -Wconversion attributes those to
// our call sites rather than the -isystem sol2 headers, so it fires throughout
// this file. The TU is pure sol2↔NHConfig glue with no hand-written narrowing
// (the real color arithmetic lives in color_parse.cpp, which keeps full
// -Wconversion), so silence just this warning for just this translation unit.
// Scoped in-source via pragma rather than a per-target CMake flag so it stays
// isolated to lua_config.cpp even if the target ever gains more sources.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

// Option-A Lua config front-end: evaluate a script through a global-function
// builder DSL that assembles an NHConfig. The primitives reuse the existing
// predicate parser (parsePredicateExpr) and enum converters, and replicate the
// TOML loader's field mapping exactly so `config flatten` output round-trips
// through ConfigLoader identically to hand-written TOML. See
// docs/config-scripting-lua.md.

namespace nodehammer::lua {

namespace {

// ── sol helpers ──────────────────────────────────────────────────────────────

bool isPresent(const sol::object &o) {
    const sol::type t = o.get_type();
    return t != sol::type::lua_nil && t != sol::type::none;
}

// Warn on any string key not in the section's allowlist — the Lua mirror of the
// TOML loader's warnUnknownKeys, drawing on the same lists (config_keys.hpp) so a
// typo'd or stale key is rejected identically on both front-ends.
void warnUnknownKeys(const sol::table &t, std::span<const std::string_view> known,
                     const std::string &ctx, DiagnosticList &diags) {
    for (const auto &kv : t) {
        if (kv.first.get_type() != sol::type::string) {
            continue;
        }
        const std::string key = kv.first.as<std::string>();
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            diags.warn(codes::kWarnConfigUnknownKey, std::format("{}: unknown key '{}'", ctx, key),
                       ctx);
        }
    }
}

// ── Predicate parsing (string or list-of-strings → OR) ───────────────────────

std::optional<config::PredicateExpr>
parseOnePredicate(const std::string &expr, const std::string &ctx, DiagnosticList &diags) {
    auto parsed = config::parsePredicateExpr(expr);
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
std::optional<config::PredicateExpr>
predicateFromLua(const sol::object &val, const std::string &ctx, DiagnosticList &diags) {
    if (val.get_type() == sol::type::string) {
        return parseOnePredicate(val.as<std::string>(), ctx, diags);
    }
    if (val.get_type() == sol::type::table) {
        const sol::table arr = val.as<sol::table>();
        const std::size_t n = arr.size(); // luaL_len — only meaningful for arrays
        // A predicate list must be a clean 1..n sequence. Reject map-like tables
        // or holey arrays (where the key count disagrees with the length) so a
        // mistyped list surfaces an error instead of silently dropping entries.
        std::size_t keyCount = 0;
        for ([[maybe_unused]] const auto &kv : arr) {
            ++keyCount;
        }
        if (keyCount != n) {
            diags.error(codes::kErrConfigParse,
                        std::format("{}: expected an array of predicate strings", ctx), ctx);
            return std::nullopt;
        }
        std::vector<config::PredicateExpr> operands;
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
        return config::combineOr(std::move(operands));
    }
    diags.error(codes::kErrConfigParse,
                std::format("{}: expected a predicate string or list of strings", ctx), ctx);
    return std::nullopt;
}

// ── Color array parsing (hex strings go through the shared parseHexColor) ────

config::Color colorFromArray(const sol::table &arr, config::Color base) {
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

// ── Lua table → ExtrasMap (for `extras`) ─────────────────────────────────────

config::ExtrasMap extrasFromLua(const sol::object &o) {
    using ExtrasMap = config::ExtrasMap;
    switch (o.get_type()) {
    case sol::type::boolean:
        return ExtrasMap{o.as<bool>()};
    case sol::type::number: {
        // Preserve the Lua 5.4 integer/float distinction so extras round-trip
        // like the TOML loader's int/float handling. sol2's is<int64_t> accepts
        // (and rounds) integral-valued floats, so check the actual Lua subtype
        // via lua_isinteger — 1.0 and 0.5 are both floats, only 1 is an integer.
        lua_State *L = o.lua_state();
        o.push();
        const bool isInt = lua_isinteger(L, -1) != 0;
        lua_pop(L, 1);
        if (isInt) {
            return ExtrasMap{o.as<std::int64_t>()};
        }
        return ExtrasMap{o.as<double>()};
    }
    case sol::type::string:
        return ExtrasMap{o.as<std::string>()};
    case sol::type::table: {
        const sol::table t = o.as<sol::table>();
        const std::size_t n = t.size();
        std::size_t keyCount = 0;
        for ([[maybe_unused]] const auto &kv : t) {
            ++keyCount;
        }
        // Contiguous 1..n integer keys with no extras → JSON array.
        if (n > 0 && keyCount == n) {
            ExtrasMap::Array elems;
            for (std::size_t i = 1; i <= n; ++i) {
                elems.push_back(extrasFromLua(t.get<sol::object>(i)));
            }
            return ExtrasMap::makeArray(std::move(elems));
        }
        ExtrasMap::Object members;
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
            members.emplace_back(std::move(key), extrasFromLua(kv.second));
        }
        return ExtrasMap::makeObject(std::move(members));
    }
    default:
        return ExtrasMap{};
    }
}

// ── Tessellation sub-table → Rule::Tessellation ──────────────────────────────

config::Rule::Tessellation tessFromLua(const sol::table &t, const std::string &ctx,
                                       DiagnosticList &diags) {
    warnUnknownKeys(t, config::keys::kTessellationKeys, ctx, diags);
    config::Rule::Tessellation tess;
    if (const sol::optional<bool> v = t[config::keys::kSkipGeometry]) {
        tess.skipGeometry = *v;
    }
    if (const sol::optional<bool> v = t[config::keys::kMergeDescendants]) {
        tess.mergeDescendants = *v;
    }
    if (const sol::optional<bool> v = t[config::keys::kDropCoincidentFaces]) {
        tess.dropCoincidentFaces = *v;
    }
    if (const sol::optional<bool> v = t[config::keys::kAverageMaterialStack]) {
        tess.averageMaterialStack = *v;
    }
    if (const sol::optional<int> v = t[config::keys::kMaxSegmentsCircle]) {
        tess.maxSegmentsCircle = *v;
    }
    if (const sol::optional<std::string> v = t[config::keys::kFallback]) {
        if (auto parsed = config::parseBooleanFallback(*v)) {
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

config::ConfigResult evalLuaConfig(std::string_view src, std::string_view rootKey,
                                   config::IncludeFetcher fetcher) {
    config::NHConfig cfg;
    DiagnosticList diags;
    const std::string chunkName{rootKey};

    try {
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);

        // Guard against a runaway script (e.g. `while true do end`) wedging the
        // CLI or the test/CI process: abort once the instruction budget is spent.
        // The budget is generous — real configs run a handful of loops — so only
        // pathological scripts trip it. (Design doc §9.)
        static constexpr int kInstructionBudget = 100'000'000;
        lua_sethook(
            lua.lua_state(),
            [](lua_State *L, lua_Debug *) {
                luaL_error(L, "config script exceeded the instruction budget "
                              "(possible infinite loop)");
            },
            LUA_MASKCOUNT, kInstructionBudget);

        // Include-resolution state. `keyStack.back()` is the key of the
        // currently-executing chunk, so nested include()/use() resolve against
        // the file that asked for them — `resolveIncludeKey` takes the parent
        // directory of that key, which is the same walk the TOML loader does.
        //
        // `rootDir` is the directory the root key sits in, and nothing may
        // resolve outside it. That test is on keys rather than on the
        // filesystem: there may not *be* a filesystem (a `.lua` inside a
        // .nhproj), and the guard has to hold identically either way.
        const std::string rootDir = std::filesystem::path{rootKey}.parent_path().generic_string();
        std::vector<std::string> keyStack{chunkName};
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

        // Resolve an include()/use() target to a key under the config root.
        // Rejects absolute paths and anything that escapes `rootDir` via `..`,
        // emitting a diagnostic and returning nullopt.
        //
        // `resolveIncludeKey` normalises lexically, so an escape shows up as a
        // key that is no longer under the root prefix — which is why the check
        // can be a string comparison and needs to touch no filesystem. When the
        // root key has no directory at all (`"<string>"`), `rootDir` is empty
        // and the prefix test degenerates to "must not start with ..", which is
        // the same rule.
        auto resolveUnder = [&](const std::string &rel,
                                const std::string &ctx) -> std::optional<std::string> {
            if (std::filesystem::path(rel).is_absolute()) {
                diags.error(codes::kErrConfigParse,
                            std::format("{}: absolute paths are not allowed ('{}')", ctx, rel),
                            ctx);
                return std::nullopt;
            }
            const std::string key = config::ConfigLoader::resolveIncludeKey(keyStack.back(), rel);
            const bool under = rootDir.empty() ? !key.starts_with("..")
                                               : (key.size() > rootDir.size() &&
                                                  key.compare(0, rootDir.size(), rootDir) == 0 &&
                                                  key[rootDir.size()] == '/');
            if (!under) {
                diags.error(codes::kErrConfigParse,
                            std::format("{}: '{}' resolves outside the config root", ctx, rel),
                            ctx);
                return std::nullopt;
            }
            return key;
        };

        // Bytes for a resolved key, as text. The fetcher is the front end's
        // only contact with anything outside the script.
        auto fetchText = [&](const std::string &key) -> std::optional<std::string> {
            const auto bytes = fetcher(key);
            if (!bytes) {
                return std::nullopt;
            }
            return std::string{reinterpret_cast<const char *>(bytes->data()), bytes->size()};
        };

        // ── config { hoist_orphans=, deduplicate_shapes= } ───────────────────
        lua.set_function("config", [&](sol::table t) {
            warnUnknownKeys(t, config::keys::kConfigFlagKeys, "config", diags);
            if (const sol::optional<bool> v = t[config::keys::kHoistOrphans]) {
                cfg.hoistOrphans = *v;
            }
            if (const sol::optional<bool> v = t[config::keys::kDeduplicateShapes]) {
                cfg.deduplicateShapes = *v;
            }
        });

        // ── export(name, { … }) ──────────────────────────────────────────────
        lua.set_function("export", [&](const std::string &name, sol::table t) {
            const std::string ctx = "export." + name;
            config::CommonExportConfig common;
            if (const sol::optional<double> v = t[config::keys::kUnitScale]) {
                common.unitScale = *v;
            }
            // Same schema table the TOML loader walks (config_keys.hpp), so a
            // format-specific key cannot be enforced on one front-end and not the
            // other. A misplaced key is known-but-in-the-wrong-place: it gets the
            // precise error and joins the valid set, so the generic unknown-key
            // pass below does not report it a second time. Only the message
            // differs from the loader's, because the syntax does.
            std::vector<std::string_view> validKeys;
            for (const auto &kd : config::keys::kExportKeys) {
                validKeys.push_back(kd.key);
                if (config::keys::exportKeyAllowed(kd, name) || !t[kd.key].valid()) {
                    continue;
                }
                std::string validFmts;
                for (const auto &f : kd.formats) {
                    if (!validFmts.empty()) {
                        validFmts += " or ";
                    }
                    validFmts += std::format("export('{}')", f);
                }
                diags.error(codes::kErrConfigParse,
                            std::format("'{}' is only valid under {}, not export('{}')", kd.key,
                                        validFmts, name),
                            ctx);
            }

            if (name == "obj") {
                warnUnknownKeys(t, validKeys, ctx, diags);
                config::ObjExportFormatConfig c;
                c.common = common;
                cfg.exportFormats[name] = c;
            } else if (name == "gltf" || name == "glb") {
                warnUnknownKeys(t, validKeys, ctx, diags);
                config::GltfExportFormatConfig c;
                c.common = common;
                if (const sol::optional<bool> v = t[config::keys::kBakeUnitScale]) {
                    c.bakeUnitScale = *v;
                }
                if (const sol::optional<bool> v = t[config::keys::kMultiScene]) {
                    c.multiScene = *v;
                }
                if (const sol::optional<std::string> v = t[config::keys::kSceneNameSeparator]) {
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
            const std::string ctx = std::format("material.{}", name);
            warnUnknownKeys(t, config::keys::kMaterialKeys, ctx, diags);
            config::MaterialDef def;
            def.name = name;
            const sol::object bc = t[config::keys::kBaseColor];
            if (bc.get_type() == sol::type::table) {
                def.baseColor = colorFromArray(bc.as<sol::table>(), def.baseColor);
            } else if (bc.get_type() == sol::type::string) {
                const std::string hex = bc.as<std::string>();
                if (auto parsed = config::parseHexColor(hex)) {
                    def.baseColor = *parsed;
                } else {
                    diags.warn(codes::kWarnConfigUnknownKey,
                               std::format("material '{}': invalid hex color '{}'; expected "
                                           "#RRGGBB or #RRGGBBAA",
                                           name, hex),
                               ctx);
                }
            }
            if (const sol::optional<float> v = t[config::keys::kMetallic]) {
                def.metallic = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kRoughness]) {
                def.roughness = *v;
            }
            if (const sol::optional<bool> v = t[config::keys::kDoubleSided]) {
                def.doubleSided = *v;
            }
            if (const sol::object em = t[config::keys::kEmissive];
                em.get_type() == sol::type::table) {
                def.emissive = colorFromArray(em.as<sol::table>(), def.emissive);
            }
            if (const sol::optional<std::string> am = t[config::keys::kAlphaMode]) {
                if (auto parsed = config::parseAlphaMode(*am)) {
                    def.alphaMode = *parsed;
                } else {
                    diags.warn(codes::kWarnConfigUnknownKey,
                               std::format("material '{}': unknown alpha_mode '{}'; expected "
                                           "opaque, mask, or blend",
                                           name, *am),
                               ctx);
                }
            }
            if (const sol::optional<float> v = t[config::keys::kAlphaCutoff]) {
                def.alphaCutoff = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kIor]) {
                def.ior = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kTransmission]) {
                def.transmission = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kClearcoat]) {
                def.clearcoat = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kClearcoatRoughness]) {
                def.clearcoatRoughness = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kAnisotropy]) {
                def.anisotropy = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kAnisotropyRotation]) {
                def.anisotropyRotation = *v;
            }
            if (const sol::optional<float> v = t[config::keys::kSpecular]) {
                def.specularFactor = *v;
            }
            if (const sol::object sc = t[config::keys::kSpecularColor];
                sc.get_type() == sol::type::table) {
                def.specularColor =
                    colorFromArray(sc.as<sol::table>(), config::Color{1.0f, 1.0f, 1.0f, 1.0f});
            }
            cfg.materials.push_back(std::move(def));
        });

        // ── keep{…} / drop{…} — string or list-of-strings ────────────────────
        auto addSelection = [&](config::SelectionAction action, const sol::object &v) {
            if (auto p = predicateFromLua(
                    v, action == config::SelectionAction::KeepIf ? "keep" : "drop", diags)) {
                config::SelectionRule rule;
                rule.action = action;
                rule.predicate = std::move(*p);
                cfg.selection.push_back(std::move(rule));
            }
        };
        lua.set_function("keep",
                         [&](sol::object v) { addSelection(config::SelectionAction::KeepIf, v); });
        lua.set_function("drop",
                         [&](sol::object v) { addSelection(config::SelectionAction::DropIf, v); });

        // ── rule{ match=, material=, tessellation=, extras= } ────────────────
        lua.set_function("rule", [&](sol::table t) {
            warnUnknownKeys(t, config::keys::kRuleKeys, "rule", diags);
            config::Rule rule;
            const sol::object m = t[config::keys::kMatch];
            if (m.get_type() == sol::type::string || m.get_type() == sol::type::table) {
                if (auto p = predicateFromLua(m, "rule.match", diags)) {
                    rule.match = std::move(*p);
                }
            } else if (isPresent(m)) {
                diags.error(codes::kErrConfigParse,
                            "rule.match: expected a predicate string or list of strings", "rule");
            }
            // Absent match → matches all nodes (rule.match stays nullopt).

            if (const sol::optional<std::string> mat = t[config::keys::kMaterialRef]) {
                rule.material = *mat;
            }
            if (const sol::object te = t[config::keys::kTessellation];
                te.get_type() == sol::type::table) {
                rule.tessellation = tessFromLua(te.as<sol::table>(), "rule.tessellation", diags);
            }
            if (const sol::object ex = t[config::keys::kExtras];
                ex.get_type() == sol::type::table) {
                rule.extras = extrasFromLua(ex);
            }
            cfg.rules.push_back(std::move(rule));
        });

        // ── defaults{ tessellation=, extras= } ───────────────────────────────
        lua.set_function("defaults", [&](sol::table t) {
            warnUnknownKeys(t, config::keys::kDefaultsKeys, "defaults", diags);
            if (const sol::object te = t[config::keys::kTessellation];
                te.get_type() == sol::type::table) {
                cfg.tessellationDefaults =
                    tessFromLua(te.as<sol::table>(), "defaults.tessellation", diags);
            }
            if (const sol::object ex = t[config::keys::kExtras];
                ex.get_type() == sol::type::table) {
                cfg.extrasDefaults = extrasFromLua(ex);
            }
        });

        // ── include(path) — run a fragment into the shared cfg ───────────────
        lua.set_function("include", [&](const std::string &rel) {
            const auto key = resolveUnder(rel, "include");
            if (!key) {
                return;
            }
            if (includeStack.contains(*key)) {
                diags.error(codes::kErrConfigParse,
                            std::format("include cycle detected: '{}'", rel), *key);
                return;
            }
            auto contents = fetchText(*key);
            if (!contents) {
                diags.error(codes::kFatalImportFileNotFound,
                            std::format("include target not found: '{}'", rel), *key);
                return;
            }
            includeStack.insert(*key);
            keyStack.push_back(*key);
            bool ok = false;
            runChunk(*contents, *key, ok);
            keyStack.pop_back();
            includeStack.erase(*key);
        });

        // ── use(path) — import a library value, cached + deep-frozen ──────────
        lua.set_function("use", [&](const std::string &rel) -> sol::object {
            const auto resolved = resolveUnder(rel, "use");
            if (!resolved) {
                return sol::lua_nil;
            }
            const std::string key = *resolved;
            if (const auto it = useCache.find(key); it != useCache.end()) {
                return it->second;
            }
            if (useStack.contains(key)) {
                diags.error(codes::kErrConfigParse, std::format("use cycle detected: '{}'", rel),
                            key);
                return sol::lua_nil;
            }
            auto contents = fetchText(key);
            if (!contents) {
                diags.error(codes::kFatalImportFileNotFound,
                            std::format("use target not found: '{}'", rel), key);
                return sol::lua_nil;
            }
            useStack.insert(key);
            keyStack.push_back(key);
            bool ok = false;
            sol::object mod = runChunk(*contents, key, ok);
            keyStack.pop_back();
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

    return config::ConfigResult{std::move(cfg), std::move(diags)};
}

} // namespace nodehammer::lua

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
