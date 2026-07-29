# Scripted configuration (Lua) — design exploration

**Status:** Option A implemented — the `config-lua` CLI command (backed by the
native-only `nodehammer_lua` library) evaluates a Lua script into an `NHConfig`
and emits flattened TOML. Option B (embedding the engine so scripts run inside
the app/browser) remains future work. This document captures the design space so
the remaining trade-offs stay mapped.

The existing TOML config is described operationally in
[predicate-expressions.md](predicate-expressions.md); the parser lives in
[`config_loader.cpp`](../src/config/config_loader.cpp) and the data model in
[`config_ast.hpp`](../src/config/config_ast.hpp). This doc
assumes familiarity with both.

---

## 1. Motivation

The config is a detector-geometry description: material definitions, selection
rules, and predicate-matched tessellation rules. The ODD fixtures show the
problem TOML can't solve — **systematic repetition that TOML has no way to
factor out**:

- [`odd/materials.toml`](../fixtures/configs/odd/materials.toml) hand-unrolls a
  14-entry `source-material → render-material` table into 14 `[[rules]]`
  blocks, including two spelling-variant `||` cases.
- [`odd/tracker.toml`](../fixtures/configs/odd/tracker.toml) repeats an
  identical selection/rule structure three times (Pixels / ShortStrips /
  LongStrips) with an *irregular* naming convention (container `Pixels`, but
  grouping nodes `PixelLayer` / `PixelEndcapN`). The irregularity is exactly
  what makes copy-paste error-prone.
- [`odd/calorimeters.toml`](../fixtures/configs/odd/calorimeters.toml) does the
  same for ECal/HCal × {Barrel, Endcap_A, Endcap_B}.

`include = [...]` gives *file composition* but no parameterization: no loops,
no computed values, no per-subsystem defaults, no table-driven rule generation.
A real language closes that gap.

**Note on where the value is.** If the node names were perfectly regular,
glob patterns alone would nearly suffice. It is the naming irregularity
(`Pixels`/`PixelLayer`/`PixelEndcapN`, `GroundOrHvMix`/`GroundOrHVMix`) that
makes a language pay off: encode the convention *once* in a helper, apply it
as data.

---

## 2. The integration seam

The pipeline has a clean, single seam:

```
.toml ──parse──> toml::table ──resolve include──> merged table ──parseTable()──> NHConfig ──> everything downstream
```

Nothing downstream of `NHConfig` (selection, tessellation, export, validation,
the `configToToml` writer) knows about TOML. A scripting front-end therefore
only has to **produce an `NHConfig`**; the entire rest of the system is
untouched. The entry points it would sit beside are
`ConfigLoader::loadFromString` / `loadFromFile` / `parseAndMerge`
([`config_loader.hpp`](../src/config/config_loader.hpp)).

There is already an `NHConfig → TOML` writer (`configToToml`) and a
`config-flatten` command, so the data model round-trips *out* to TOML today.

---

## 3. Engine choice: Lua

The deciding constraint is **wasm**. The config loader is compiled into
`nodehammer_lib`, which is in the Emscripten closure — both the web viewer and
the headless `nodehammer-compute` worker link it (see
[`CMakeLists.txt`](../CMakeLists.txt), `nodehammer_lib` target and the
`EMSCRIPTEN` branches). Whatever engine is embedded ships in the `.wasm` and
must cross-compile under emcc.

| Engine | wasm | Binary cost | Notes |
|---|---|---|---|
| **Lua 5.4** | trivial (pure C) | ~200–300 KB | Plain Lua; LuaJIT's JIT is useless in wasm. `sol2` for the C++ binding. |
| QuickJS | yes | ~0.6–1 MB | Real modern JS, but 2–3× the binary. |
| Duktape | yes | ~350 KB | Smaller JS, older ES subset. |
| V8 / Node | **no** | — | Not embeddable in this target. |

**Lua is the pick** unless the user base specifically writes JS and never will
write Lua — in which case "JS" realistically means QuickJS/Duktape, not the JS
people expect, at a meaningfully larger binary.

Dependency wiring is dual: toml++ is pinned in both
[`conanfile.py`](../conanfile.py) and
[`cmake/Dependencies.cmake`](../cmake/Dependencies.cmake). A Lua dep would need
adding in both to keep the Conan-CI and FetchContent-local build paths in sync
(Conan Center has `lua/5.4.x`).

---

## 4. Two integration options (same API surface)

The **DSL surface is identical** either way — only the C++ backend differs — so
this choice does not constrain ergonomics, and a prototype built as A promotes
to B without users rewriting anything.

- **Option A — codegen / pre-pass.** The script is a *generator* that emits
  TOML (or merged JSON) which the existing loader consumes unchanged. No engine
  in `nodehammer_lib` / wasm. Extra build step; no in-browser scripting.
  Lowest risk.
- **Option B — embedded.** Lua lives in `nodehammer_lib`; a new
  `ConfigLoader::loadFromLua` runs the script and returns
  `ConfigResult{cfg, diags}`, a peer of `loadFromString`. Works everywhere the
  loader works, including in-browser. Costs binary size everywhere and requires
  a sandbox.

**Recommendation:** start with **A + Lua** (delivers the loop/parameterization
win at near-zero risk and no wasm bloat), keep the door open to B. The one
decision that forces the choice: *must config scripts execute inside the
app/browser?* → B. *Is a build-time generation step acceptable?* → A.

---

## 5. DSL design: a global-function builder

The chosen shape is a **global-function builder DSL** — the same idiom as
CMake / Premake / `build.gradle`. The script *is* the config; calling
`material{…}`, `rule{…}`, `keep{…}` appends to a config being assembled.
Everything else is ordinary Lua (variables, `for`, functions, tables,
closures). The primitives stay a small fixed vocabulary that maps 1:1 onto
`NHConfig`; the *language* supplies the abstraction TOML lacks.

### 5.1 Primitive surface (the entire vocabulary)

```lua
-- top-level flags  → NHConfig.hoistOrphans / deduplicateShapes
config { hoist_orphans = true, deduplicate_shapes = true }

-- export overrides → NHConfig.exportFormats["gltf"|"glb"|"obj"]
export("gltf", { unit_scale = 0.1, bake_unit_scale = true })

-- material def     → MaterialDef (name is the first arg)
material("silicon", { base_color = "#60666E", metallic = 1.0, roughness = 0.05 })

-- selection rule   → SelectionRule (list = OR, mirrors TOML array form)
keep { 'path ~= "**/Pixels"', 'name == "BeamPipe"' }
drop { 'tag.sensitive == "false"' }

-- unified rule     → Rule { match, material, tessellation, extras }
rule {
  match        = 'name ~= "Solenoid*"',                 -- string OR a { } list of strings (OR'd)
  material     = "silicon",                             -- optional
  tessellation = { max_segments_circle = 48, fallback = "skip" },  -- optional
  extras       = { visible = true },                    -- optional (→ nlohmann::json)
}

-- global fallback  → NHConfig.tessellationDefaults / extrasDefaults
defaults { tessellation = { max_segments_circle = 10, fallback = "skip" },
           extras       = { visible = true, opacity = 1.0 } }

-- composition (see §7)
include("odd/tracker.lua")           -- run a config fragment for its side-effects
local tk = use("lib/tracker.lua")    -- import a library value (returns, cached)
```

### 5.2 Semantic mapping

- `match` / `keep` / `drop` accept a string **or** a list of strings; a list is
  OR'd — byte-identical to today's `keep_if = [ … ]` / `match = [ … ]`. The
  existing predicate expression language (`path ~= "…" && is_leaf`, `any(…)`)
  is unchanged. Scripting sits *above* the matcher, generating rule lists, never
  replacing it.
- Rule / selection **order is preserved by call order** (matters: material
  resolution is last-match-wins).
- `base_color` accepts `"#RRGGBB[AA]"` or `{r,g,b,a}`, mirroring the TOML field.

---

## 6. Key design decision: global name, never global state

The unprefixed `keep{…}` syntax (vs. `cfg:keep{…}`) does **not** require
process-global C++ state. Two things were conflated:

- a global *name* in the Lua environment (`keep` resolvable without a prefix) —
  required for the syntax, harmless;
- global *state* in C++ (a `static NHConfig` accumulator) — a re-entrancy
  footgun, and avoidable.

The syntax needs only the first. Bind the primitives to **per-run closures**
that capture the run's builder: the name is global *to the script*; the state
it touches is local *to the call* and captured lexically.

```cpp
ConfigResult loadFromLua(std::string_view src, std::string_view name, IncludeFetcher fetcher) {
    sol::state lua;                         // born and dies with the call → re-entrant by construction
    lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math);

    NHConfig cfg;
    DiagnosticList diags;

    // Unprefixed in the script, but each closes over THIS run's cfg. No static state.
    lua.set_function("keep",     [&](sol::table t){ addSelection(cfg, KeepIf, t, diags); });
    lua.set_function("rule",     [&](sol::table t){ cfg.rules.push_back(parseRuleTable(t, diags)); });
    lua.set_function("material", [&](std::string n, sol::table t){ cfg.materials.push_back(parseMaterial(n, t, diags)); });
    lua.set_function("include",  [&](std::string rel){
        if (auto bytes = fetcher(resolveIncludeKey(name, rel)))
            lua.safe_script(asStringView(*bytes), rel);   // same closures ⇒ appends to same cfg
    });
    // … drop / defaults / export / config / use …

    lua.safe_script(src, name);
    return { std::move(cfg), std::move(diags) };
}
```

Because both the `sol::state` and the `cfg` are stack locals of the call,
concurrent loads can't collide and there is **nothing to reset** — the "re-run
doubles everything" hazard cannot occur. This matters concretely: the viewer
hot-reloads configs ([`build_session.cpp`](../src/viewer/build_session.cpp)),
so re-entrancy is not hypothetical.

### 6.1 Why a builder object was rejected

A `cfg:rule{…}` method form (explicit builder threaded through) is also
re-entrant, but costs the unprefixed ergonomics that are the whole reason to
pick a global DSL. A *pure returned table* (`return { rules = {…} }`) is clean
but taxes the two things this codebase leans on hardest:

- **includes** — a returned value forces the parent to re-implement
  `mergeToml`'s last-wins/concat merge semantics in Lua and call it at every
  include site;
- **diagnostics** — the loader is built around `DiagnosticList` with precise
  `file:line:col`; a single returned blob loses per-call provenance, whereas
  each `keep`/`rule` call is a natural attribution point (`debug.getinfo`).

Per-run closures keep unprefixed syntax **and** re-entrancy **and** per-call
diagnostics with zero ambient state. That is the resolution: *global name,
per-run state.*

### 6.2 Rejected alternative: thread-local

A `thread_local Builder* current` read by static `lua_CFunction`s also works
and lets you register the functions once, but reintroduces ambient mutable
state with save/restore-on-nesting and restore-on-error discipline. sol2 makes
capturing lambdas trivial, so there is no reason to take on that discipline
here.

---

## 7. Composition: `include` vs `use`

Two complementary primitives, because "merge a config fragment" and "import a
library" are different operations:

| | `include(path)` | `use(path)` |
|---|---|---|
| Purpose | merge a config fragment | import a library value |
| Return value | discarded | captured & bound to a local |
| Runs | every call, **in order** | once, **cached** by key |
| Namespacing | shared globals | local binding, no pollution |
| Analogue | TOML `include=` | Lua `require` / JS `import` |

`include` as written discards the child's return value, so it cannot cleanly
"import a function" — that would only work by the child leaking a *global*
function (fragile, order-dependent, collision-prone). The fix is a second
primitive that returns and caches — Lua's `require`, re-implemented over the
fetcher (stock `require`/`package` are disabled for the sandbox):

```cpp
auto cache = std::make_shared<std::unordered_map<std::string, sol::object>>();
lua.set_function("use", [&, cache](std::string rel) -> sol::object {
    std::string key = resolveIncludeKey(name, rel);
    if (auto it = cache->find(key); it != cache->end()) return it->second;  // run-once; breaks cycles
    auto bytes = fetcher(key);
    if (!bytes) { diags.error(codes::kErrImportFileNotFound, /*…*/); return sol::lua_nil; }
    sol::object mod = lua.safe_script(asStringView(*bytes), key);           // capture the RETURN value
    mod = readonly(lua, mod);                                               // deep-freeze the import (§7.1)
    (*cache)[key] = mod;
    return mod;
});
```

Both resolve through the same `IncludeFetcher` + `resolveIncludeKey` machinery
the TOML loader uses, so `.lua` modules load through project-fs / bag / URL keys
identically to TOML includes; `use`'s cache mirrors the loader's existing
visited-set cycle detection.

**Capability per `_ENV` (optional knob).** Running each chunk under a fresh
`_ENV` lets the two primitives differ in what they may do: `include` gets the
full DSL env (it is *meant* to have side-effects); `use` can run under a
DSL-free env (`string`/`table`/`math`/`use`, but no `keep`/`rule`), forcing
imported libraries to be *pure* — they compute and return values, they cannot
secretly mutate the config. A shared env (libs may call the DSL) is also
defensible; it is a policy dial, not a limitation.

### 7.1 `constants.lua` and read-only imports

The prime `use` case is a shared-constants module — a single source of truth
imported across fragments:

```lua
-- lib/constants.lua — pure data, returns a table
return {
  endcaps = { "EndcapN", "EndcapP" },
  seg     = { coarse = 10, fine = 48 },
  palette = { silicon = "#60666E", kapton = "#9A5516", copper = "#B66A3C" },
}
```

```lua
local K = use("lib/constants.lua")
material("silicon", { base_color = K.palette.silicon, metallic = 1.0, roughness = 0.05 })
rule { match = '…', tessellation = { max_segments_circle = K.seg.fine } }
```

Because `use` is cached, `constants.lua` evaluates **exactly once** even under a
diamond import, and every importer sees the same values — bound to a local, no
globals. It is also a natural fit for the DSL-free env (§7): a constants module
never calls `keep`/`rule`, so running it pure *structurally* forbids side
effects.

The one hazard is that the cache hands back the *same* table instance, so a
careless `K.seg.fine = 12` in one consumer would leak to every other. `use`
closes this by **deep-freezing** the return value before caching, so a write
raises a loud error at the offending site instead of silently corrupting shared
state:

```lua
local function readonly(t)
  if type(t) ~= "table" then return t end
  local backing = {}
  for k, v in pairs(t) do backing[k] = readonly(v) end       -- freeze nested tables too
  return setmetatable({}, {
    __index    = backing,
    __newindex = function(_, k) error(("cannot modify read-only import (key %q)"):format(k), 2) end,
    __pairs    = function() return next, backing, nil end,    -- keep pairs() working over the proxy
    __len      = function() return #backing end,
    __metatable = false,                                       -- lock the metatable
  })
end
```

An empty proxy table (rather than guarding the real table) is required because
Lua's `__newindex` only fires for *absent* keys — a populated table's existing
keys would stay writable. Reads/`ipairs`/`#` forward through `__index`/`__len`,
and `pairs` works via `__pairs` (retained in Lua 5.4 — only `__ipairs` was
removed). The freeze lives inside `use` (implemented in C++ over sol2, or via a
tiny injected prelude); the guarantee is that imported modules are immutable, so
`constants.lua` behaves like a genuine constants file.

**This does not cap Lua.** Closures, higher-order functions, returning
functions/tables, metatables, coroutines — all intact. The only removals are
the host-I/O escape hatches (`io`, `os`, `package`, stock `require`), done for
the sandbox regardless of DSL style; module composition is re-provided as
`use`.

---

## 8. Worked examples

### 8.1 The 14-rule material map

Before: 55 lines in [`odd/materials.toml`](../fixtures/configs/odd/materials.toml)
(§ "source-material → render-material"). After:

```lua
-- source-material glob(s)  →  render material
local material_map = {
  { "Beryllium",                        "beryllium" },
  { "Silicon",                          "silicon" },
  { "Kapton",                           "kapton" },
  { "Aluminum",                         "aluminum" },
  { "CarbonFiber*",                     "carbon_fiber" },
  { "Titanium",                         "titanium" },
  { "CarbonFoam",                       "carbon_foam" },
  { "Copper",                           "copper" },
  { "TungstenDens24",                   "tungsten_dens24" },
  { "G10",                              "g10" },
  { {"GroundOrHvMix", "GroundOrHVMix"}, "ground_or_hv_mix" },  -- spelling aliases → OR
  { {"SiPcbMix", "siPCBMix"},           "si_pcb_mix" },
  { "Steel*",                           "steel" },
  { "Polystyrene",                      "polystyrene" },
}

local function match_material(src)          -- normalize scalar-or-list → OR list
  local srcs = type(src) == "table" and src or { src }
  local out = {}
  for _, s in ipairs(srcs) do out[#out+1] = ('material ~= "%s"'):format(s) end
  return out
end

for _, m in ipairs(material_map) do
  rule { match = match_material(m[1]), material = m[2] }
end
```

The alias case that was special-cased in TOML falls out of the same code path.

### 8.2 The tracker subsystem

Before: ~90 lines in [`odd/tracker.toml`](../fixtures/configs/odd/tracker.toml),
Pixels / ShortStrips / LongStrips repeated with irregular naming. After —
encode the convention once, list the subsystems as data:

```lua
local subsystems = {
  { plural = "Pixels",      layer = "PixelLayer",      endcap = "Pixel" },
  { plural = "ShortStrips", layer = "ShortStripLayer", endcap = "ShortStrip" },
  { plural = "LongStrips",  layer = "LongStripLayer",  endcap = "LongStrip" },
}

local function path(g) return 'path ~= "' .. g .. '"' end
local function leaf(g) return path(g) .. ' && is_leaf' end

for _, s in ipairs(subsystems) do
  local eN, eP = s.endcap .. "EndcapN", s.endcap .. "EndcapP"

  -- keep the container spine
  keep {
    path("**/" .. s.plural),
    path("**/" .. s.plural .. "/*Barrel"),
    path("**/" .. s.plural .. "/*EndcapN"),
    path("**/" .. s.plural .. "/*EndcapP"),
  }

  -- keep grouping nodes and their sensitive leaves
  keep {
    path("**/" .. s.layer .. "*"),
    path("**/" .. s.layer .. "*/stave*"),
    leaf("**/" .. s.layer .. "*/stave*/**"),
    path("**/" .. eN .. "*/disk*"),  leaf("**/" .. eN .. "*/disk*/**"),
    path("**/" .. eP .. "*/disk*"),  leaf("**/" .. eP .. "*/disk*/**"),
  }

  -- structural container nodes carry no geometry
  rule {
    match = {
      path("**/" .. s.plural),
      path("**/" .. s.plural .. "/*Barrel"),
      path("**/" .. s.layer .. "*"),
      path("**/" .. s.plural .. "/*EndcapN"),
      path("**/" .. s.plural .. "/*EndcapP"),
    },
    tessellation = { skip_geometry = true },
  }

  -- stave / disk groups: merge descendants into one mesh
  rule {
    match = {
      path("**/" .. s.layer .. "*/stave*"),
      path("**/" .. eN .. "*/disk*"),
      path("**/" .. eP .. "*/disk*"),
    },
    tessellation = { merge_descendants = true, max_segments_circle = 10, fallback = "fail" },
  }
end

-- non-repeating tail (endcap support rings) stays literal
rule {
  match = { path("**/PixelEndcap*/**/DiskSupport_*"),
            path("**/*StripEndcap*/**/RingSupport*") },
  tessellation = { max_segments_circle = 48 },
}
```

Adding a 4th subsystem is **one line** instead of ~30 across three sections.

### 8.3 Top-level `odd.lua`

```lua
config { hoist_orphans = true }
export("gltf", { unit_scale = 0.1, bake_unit_scale = true })   -- blender
export("obj",  { unit_scale = 1.0 })

include("odd/base.lua")        -- drop-all baseline
include("odd/materials.lua")
include("odd/tracker.lua")
include("odd/calorimeters.lua")
include("odd/muon.lua")

keep { 'name == "BeamPipe"' }
rule { match = 'path ~= "/world"',    tessellation = { skip_geometry = true } }
rule { match = 'name ~= "Solenoid*"', tessellation = { max_segments_circle = 48 } }

defaults { tessellation = { max_segments_circle = 10, fallback = "skip" },
           extras       = { visible = true, opacity = 1.0 } }
```

---

## 9. Sandboxing

Required either way; mandatory for B in the shared viewer, where a `.lua` may be
untrusted:

- Open only `base` / `string` / `table` / `math`. No `io`, `os`, `package`, or
  raw `require`; `include` / `use` are the vetted I/O primitives, routed through
  the fetcher.
- Fresh environment per run *is* the sandbox — you enumerate exactly which
  globals exist and nothing leaks between loads.
- Add a `lua_sethook` instruction-count guard so a runaway `while true do` in an
  untrusted script can't wedge the build thread.

This is the one genuinely new attack surface versus toml++, which cannot execute
anything.

---

## 10. Constraints and open questions

1. **One-way by construction.** `configToToml` / `config-flatten` can still emit
   flattened TOML from the *result*, but there is no script ← config direction.
   Inherent to any Turing-complete config; worth stating, not a defect.
2. **A vs B** is the load-bearing decision (§4): in-browser execution → B;
   build-time generation acceptable → A. Everything else follows.
3. **Enforced-pure `use` or shared-env `use`** (§7) — pick a default.
4. **Coexistence** — should `.lua` and `.toml` mix in one include tree during
   migration? The fetcher/key machinery allows it; `include("x.toml")` from Lua
   would need a bridge that runs the TOML parser and merges the resulting
   `NHConfig` (or emits into the shared builder). Decide whether that's in scope
   or whether a project is all-Lua or all-TOML.
5. **Effort:** Option A is a bounded, self-contained addition — a Lua state, ~8
   bound primitives reusing the existing `parseMaterial`/`parseRuleTable`
   helpers, `include`/`use`, a sandbox, and a `.lua`-detection dispatch in
   `ConfigLoader`. Option B is the same plus shipping the engine in every target
   (incl. wasm build/CMake/Conan wiring) and the sandbox hardening that in-browser
   execution demands.
