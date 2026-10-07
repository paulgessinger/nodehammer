# Lua config builder

Use Lua for repeated subsystem rules, material maps, or computed settings.
It produces the same config model as TOML. Read
[config-format.md](config-format.md) for matching, precedence, and field values.
The CLI selects Lua by the `.lua` extension, including during project packing.

## Builder vocabulary

| Call | TOML equivalent |
|---|---|
| `config { hoist_orphans = true, deduplicate_shapes = true }` | Top-level flags |
| `material("silicon", { base_color = "#60666E" })` | `[materials.silicon]` |
| `keep 'is_leaf'` / `keep { 'is_leaf', 'name ~= "BeamPipe"' }` | One `keep_if` selection rule; list is OR |
| `drop 'true'` / `drop { 'tag.sensitive == "false"' }` | One `drop_if` rule |
| `rule { match = 'is_leaf', material = "silicon" }` | One `[[rules]]` |
| `defaults { tessellation = { max_segments_circle = 24 } }` | `[defaults.tessellation]` |
| `export("gltf", { unit_scale = 0.01, bake_unit_scale = true })` | `[export.gltf]` |
| `include("tracker.lua")` | Execute a config fragment at this point |
| `local lib = use("lib/helpers.lua")` | Evaluate and cache a returned library value |

`rule` also accepts `tessellation = { ... }` and `extras = { ... }`.
`match` may be a string or an OR-list of strings, or omitted to match all.
`defaults` also accepts `extras`. Colors use Lua arrays such as
`{ 0.2, 0.3, 0.4, 1.0 }` instead of TOML square brackets.

The script calls builders; returning a config table alone does not configure
anything. Predicates are strings parsed by nodehammer, **not Lua expressions or
callbacks**. Write `'is_leaf && tag.sensitive == "true"'`, not Lua `and` inside
the string. Lua keep/drop have no separate `scope` argument or structured
predicate-table form: express scope as `path ~= "..." && (...)`.

## Runnable equivalent of the TOML starting point

Save this as `scene.lua`. As in the TOML example, replace the assumed paths
and source material after inspecting the detector.

```lua
config { hoist_orphans = true, deduplicate_shapes = true }

material("silicon", {
  base_color = "#60666E", metallic = 0.8, roughness = 0.2,
})

drop 'true'
keep { 'path ~= "/World/Tracker"', 'path ~= "/World/Tracker/**"' }

rule {
  match = 'path ~= "/World" || path ~= "/World/Tracker"',
  tessellation = { skip_geometry = true },
}
rule { match = 'material ~= "Silicon"', material = "silicon" }

defaults {
  tessellation = { max_segments_circle = 24, fallback = "skip" },
  extras = { visible = true },
}
```

```bash
nodehammer config validate -c scene.lua
nodehammer config flatten -c scene.lua -o scene.flat.toml
nodehammer config validate -c scene.flat.toml
```

Flattening captures evaluated rules, not comments, loops, functions, or module
boundaries. Keep the Lua source as the editable version; `convert`, `config
validate`/`flatten` and project packing all accept it directly.

## Repetition and composition

Encode actual naming differences as data; do not infer regular names from one
subsystem. This fragment illustrates selection for two **assumed** containers:

```lua
local subsystem_paths = { "/World/Tracker", "/World/Calorimeter" }
for _, path in ipairs(subsystem_paths) do
  keep {
    ('path ~= "%s"'):format(path),
    ('path ~= "%s/**"'):format(path),
  }
end
```

Use `ipairs` over ordered arrays when generating rules; `pairs` iteration
order is unsuitable for precedence-sensitive rules. Builder calls append
selection and styling rules in execution order, including calls in fragments.

`include` executes each time it is called, sharing the builder and Lua
environment. Repeating an include repeats its rules; unlike TOML, it is not
deduplicated. Prefer local variables in fragments. `use` runs a module once per
resolved path per config evaluation and returns its cached value; returned
tables are recursively read-only. Use it for helper functions and constants.
Do not rely on modules being side-effect-free by enforcement: they share the
builder environment too.

Paths are relative to the calling file and must stay under the entry config's
directory. Cycles fail. Both mechanisms evaluate Lua, not TOML. Use these
mechanisms instead of filesystem I/O or `require`; the same files can then
resolve inside a packaged project. Available libraries are base, string,
table, and math, with an instruction budget; there is no supported host-I/O
or external-package workflow.

Define each material name once. Repeated `export(format, ...)` replaces that
format's settings; it does not merge individual fields. A later
`defaults { tessellation = ... }` replaces the tessellation-default block,
and likewise for extras. Keep shared defaults together instead of relying on
TOML-style deep merging between builder calls.

For an existing detector port, flatten both the TOML and Lua configurations
and compare the results. This catches missing drop-all baselines, changed rule
ordering, and omitted settings before rendering. Repository examples include
`fixtures/configs/lua/parity.lua` and `fixtures/configs/lua/odd.lua`; verify
the current flattened output rather than assuming the historical examples
still have identical content.
