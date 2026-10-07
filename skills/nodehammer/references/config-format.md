# Scene config format (TOML and shared semantics)

A config selects and styles existing geometry; it does not define a detector's
solids or placements. Input files, output paths, and output formats belong to
the CLI (`-i`, `-o`), not to config keys.

## A complete TOML starting point

This example assumes inspection found `/World/Tracker` and source material
`Silicon`. Replace those literals with the detector's actual paths/materials.
It keeps the tracker subtree, suppresses its container solid, and styles silicon.

```toml
hoist_orphans = true
deduplicate_shapes = true

[materials.silicon]
base_color = "#60666E"
metallic = 0.8
roughness = 0.2

[[selection_rules]]
drop_if = 'true'

[[selection_rules]]
keep_if = ['path ~= "/World/Tracker"', 'path ~= "/World/Tracker/**"']

[[rules]]
match = 'path ~= "/World" || path ~= "/World/Tracker"'
[rules.tessellation]
skip_geometry = true

[[rules]]
match = 'material ~= "Silicon"'
material = "silicon"

[defaults.tessellation]
max_segments_circle = 24
fallback = "skip"

[defaults.extras]
visible = true
```

Place top-level scalars and `include` before any TOML table header; otherwise
they belong to the current table. `[rules.tessellation]` and `[rules.extras]`
belong to the most recent `[[rules]]`.

## Selection and rule resolution

| Setting | Meaning |
|---|---|
| `hoist_orphans` | Default false. True reparents kept descendants of dropped nodes to the nearest kept ancestor (or root), preserving world placement. The root stays. |
| `deduplicate_shapes` | Default true. Deduplicates identical shape definitions. |
| `[[selection_rules]]` | Exactly one `keep_if` or `drop_if`, plus optional `scope` (a path glob pre-filter). |
| `[[rules]]` | Optional `match`, `material`, `tessellation`, and `extras`. Omitted `match` matches every node. These rules do not select nodes. |
| `[defaults.tessellation]` | Fallback per field, below every matching rule. |
| `[defaults.extras]` | Fallback object when no matching rule supplies extras. |

All nodes start **kept**. Selection rules run in order; the last matching
keep/drop action wins for each node. A keep-only list does not form an allowlist:
start with `drop_if = 'true'` to select only named parts. Keeping a container
does not automatically keep its descendants. With `hoist_orphans = false`, a
kept child below a dropped parent is force-dropped with NH0400; keep the complete
ancestor chain, including the root, or use hoisting deliberately.

For retained geometry, resolution differs by concern:

- **Material:** last matching rule that sets `material` wins. The value names a
  defined render material. Without an assignment, source material styling is used.
- **Tessellation:** last matching rule wins **per field**, then defaults fill
  unset fields. A later rule that only changes segments retains an earlier merge
  setting. Use explicit false to undo a boolean setting.
- **Extras:** the **first** matching rule with extras wins as a whole object.
  Objects are not merged with later rules or defaults. Extras are export/viewer
  metadata, not a substitute for selection or material alpha settings.

Put fallback tessellation in defaults, not a final unconditional rule that
overrides all the specific settings. An unconditional material rule at the end
similarly replaces earlier assignments.

## Predicates

`keep_if`, `drop_if`, and `match` accept an expression string or an array of
expression strings (OR). Use single-quoted TOML strings around expressions whose
values are double-quoted. Lua uses the same expression language inside strings.

| Expression | Matches |
|---|---|
| `true`, `false` | All / no nodes |
| `is_leaf` | Node with no children at evaluation time |
| `name ~= "sensor*"` | Node name glob |
| `path ~= "**/Tracker/**"` | Original imported path glob, including after hoisting |
| `material ~= "Silicon*"` | Source material name, not a configured render-material name |
| `tag.sensitive` | Tag exists, even if its value is "false" |
| `tag.sensitive == "true"` | Exact string tag value |

Combine with `!`, `&&`, `||` (highest to lowest precedence), parentheses,
`any(a, b, ...)`, or `all(a, b, ...)`. Tag values are strings; tag identifiers
use letters/digits/underscores and cannot start with a digit. `!=` negates a
comparison. For name/path/material, `==` is also implemented as glob matching;
prefer `~=` when using wildcards.

Globs are case-sensitive: `*` stays within a path segment, `**` crosses
slashes; `?`, character classes, and regular expressions are not supported.
`**/Tracker/**` matches descendants; include `**/Tracker` separately for the
container itself. Root spelling (`/world` versus `/World`) comes from the input.

TOML also accepts structured predicates: `{ type = "path_glob", pattern = "..." }`,
`name_glob` / `material_glob` with `pattern`, `tag` with `key` and optional
`value`, `is_leaf`, `bool` with boolean `value`, `and` / `or` with a list
of structured `operands`, and `not` with one structured `operand`. This is
useful for tag keys that cannot be expressed using dot notation.

## Materials

Define `[materials.NAME]`; reference that name with `material = "NAME"` in a
rule. Defining a material alone does not assign it to any geometry.

| Fields | Values |
|---|---|
| `base_color` | Hex "#RRGGBB" / "#RRGGBBAA", or numeric [r, g, b] / [r, g, b, a] |
| `metallic`, `roughness` | Numeric factors, normally 0–1 |
| `emissive`, `specular_color` | Numeric linear RGB arrays [r, g, b] |
| `double_sided` | Boolean, default false |
| `alpha_mode`, `alpha_cutoff` | "opaque" (default), "mask", or "blend"; cutoff for mask (default 0.5) |
| `ior`, `transmission`, `clearcoat`, `clearcoat_roughness`, `anisotropy`, `anisotropy_rotation`, `specular` | Optional PBR extension values; anisotropy rotation is in radians |

Hex RGB is converted from sRGB to linear; numeric color arrays are already
linear. Alpha is linear in both. To make a translucent material, set alpha
below 1 **and** `alpha_mode = "blend"`; lowering alpha in opaque mode is
insufficient. Extension support depends on the export/viewer consuming it.

## Tessellation

All these fields work in `[rules.tessellation]` and `[defaults.tessellation]`.

| Field | Effect |
|---|---|
| `skip_geometry` | Suppress this node's own solid while retaining its hierarchy/descendants; useful for world and envelope containers |
| `merge_descendants` | Merge retained descendant geometry into a group; assign to a stave/disk/module container, not every leaf |
| `max_segments_circle` | Positive integer controlling curved-shape resolution; built-in default 64 |
| `fallback` | Unsupported boolean handling: "skip" (default), "bbox" (bounding-box proxy), "fail" |
| `drop_coincident_faces` | Remove coincident internal triangle pairs on merged opaque stacks |
| `average_material_stack` | Compute merged-stack average data for viewer filtering of distant material bands |

Boolean switches default false. Merging changes mesh granularity, so retain
separate groups when per-part interaction matters. `skip_geometry` takes
precedence over merging on the same node; clear an inherited skip setting on a
merge group. The last two options require
merging on the same nodes. Use coincident-face removal only for opaque geometry
whose interior will not be exposed by the viewer's shader angle-cut preview;
transparent stacks need their interior faces.

## Export overrides

`[export.gltf]`, `[export.glb]`, and `[export.obj]` accept `unit_scale`.
glTF/GLB additionally accept `bake_unit_scale` (boolean), `multi_scene`
(boolean), and `scene_name_separator` (string). OBJ always bakes its scale;
do not put `bake_unit_scale` in `[export.obj]`.

GLB uses `[export.gltf]` only if `[export.glb]` is absent; an existing GLB
table does not inherit missing fields from the glTF table. Establish source
units and target expectations before overriding scale (cm to metres: 0.01;
mm to metres: 0.001). Do not copy another detector's DCC-specific scale blindly.

## TOML composition

Use top-level `include = "materials.toml"` or
`include = ["base.toml", "materials.toml", "tracker.toml"]`. Paths resolve
relative to the file containing the include. Nested includes work; repeated
files in a diamond are merged once, and cycles fail. TOML includes are TOML;
Lua composition is described in [lua-config.md](lua-config.md).

Included rule arrays precede the parent's rules, in include order. Put a
drop-all baseline first, followed by subsystem keeps. Tables merge recursively
and arrays concatenate (including color arrays). **Currently conflicting
scalars keep the first value**, including an included value before a parent's
value; do not rely on parent scalar overrides. Define each shared material,
export setting, and default in one place, then use ordered rules for overrides.
Inspect `config flatten` to confirm the effective result.

When maintaining this reference in the source repository, check
`src/config/config_keys.hpp`, `config_loader.cpp`, `config_ast.hpp`,
`src/selection/selector.cpp`, `src/tessellation/tessellation_pass.cpp`, and
`src/export_resolve.cpp`. Installed copies of this skill are self-contained.
