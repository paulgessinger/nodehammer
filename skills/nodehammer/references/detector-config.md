# Author a config for a detector

The deliverable is a config grounded in the supplied geometry, with an explicit
selection/styling purpose and evidence that it produces the intended scene.
Preserve an existing format and conventions. For a new config, use TOML for
direct rules or Lua for repeated/generated rules; the runtime supports both.

Read [config-format.md](config-format.md) when authoring either format, and
[lua-config.md](lua-config.md) for Lua. The examples use illustrative paths,
not a universal detector naming scheme.

## Establish what the scene should show

Use the user's requested subsystems, detail level, output/viewer, and interaction
needs. For example, a tracker-only overview and a full calorimeter sampling-stack
view need different selection and merge boundaries. If those choices are
unspecified, inspect first and make a stated conservative choice or ask the
smallest useful question. If the geometry is unavailable, produce a clearly
marked template and state that matching has not been verified.

Check the importer before changing config syntax. A wheel without ROOT/DD4hep
support cannot inspect those inputs; use a backend-enabled binary or an
available semantic `.nhb`/`.json` export.

## Discover the actual hierarchy

```bash
nodehammer inspect --output-format json summary -i detector.nhb
nodehammer inspect --output-format json tree -i detector.nhb --depth 3
nodehammer inspect --output-format json tags -i detector.nhb
```

Read source material names from summary and tag values from tags. Deepen the
tree for promising subtrees, using an observed path:

```bash
nodehammer inspect --output-format json tree -i detector.nhb --filter '/World/Tracker/**'
```

The inspect filter is a **path glob**, not a config predicate expression.
Inspect reports do not apply a config. Depth-limited or filtered output is only
a sample of the geometry; check actual module/leaf paths before using patterns.
Record the exact root, subsystem/container paths, repeated stave/disk/module
boundaries, leaf names, source materials, and available tags. Do not assume
`tag.sensitive`, detector names, or capitalization from another importer.

For expensive imports, cache one unconfigured semantic scene and iterate on it:

```bash
nodehammer convert -i detector.root -o detector.nhb
```

Keep this baseline unfiltered: parts removed from a cached scene cannot be
recovered by a later keep rule. Confirm source units from the input/importer
before setting export scale; inspection alone does not establish length units.

## Build selection before styling

Start with the smallest requested subsystem. For an allowlist, drop all first,
then keep the chosen containers and descendants. Either retain the full ancestor
chain or set `hoist_orphans = true`. Keep stave/disk/module containers that will
become merge boundaries. A typical detailed selection keeps these grouping
nodes plus selected leaf descendants; a simple subtree selection keeps everything
under its container and is easier to verify first.

Suppress unwanted world/envelope solids using `skip_geometry` on their exact
nodes. A broad `skip_geometry` rule on the whole subtree also suppresses the
detector's actual solids. Dropping a node, skipping its solid, and marking
extras invisible have different effects.

## Add appearance and mesh settings

Define render materials and assign them using observed source-material names
or tags. Scope assignments by path where one material occurs in several
subsystems. Hex colors are a convenient way to specify familiar sRGB colors.
Use a later, narrower material rule to override a broad assignment.

Start with moderate circle resolution in defaults (e.g. 24); increase it for
large visible cylinders or rings where faceting matters. Choose fallback
deliberately: `skip` can omit unsupported solids, `bbox` is visibly approximate,
and `fail` is useful when missing solids would invalidate the deliverable.
Review diagnostics even after successful conversion.

Merge at repeated stave/module/disk boundaries when reducing mesh count matters
more than interacting with each constituent. Avoid merging the entire detector
as a blanket optimization. Enable coincident-face removal or material-stack
averaging only on appropriate merged stacks, after the initial scene is correct.

For larger configs, separate baseline selection, material definitions/mappings,
and subsystem rules. Keep the drop-all baseline ahead of subsystem keeps. In
TOML, define shared scalar/array settings in one place to avoid include-merge
surprises; in Lua, use ordered data tables and local helpers for irregular names.

## Verify the result in stages

```bash
nodehammer config validate -c scene.toml
nodehammer config flatten -c scene.toml -o scene.flat.toml
nodehammer config validate -c scene.flat.toml
nodehammer convert -i detector.nhb -c scene.flat.toml -o selected.nhb
nodehammer inspect --output-format json summary -i selected.nhb
nodehammer inspect --output-format json tree -i selected.nhb --depth 4
nodehammer convert -i detector.nhb -c scene.flat.toml -o preview.glb --timing
```

For Lua, use `scene.lua` in place of `scene.toml` throughout; every command
loads it directly. Validation checks configuration
structure/references, not whether paths or tags match this detector. Flattening
reveals the evaluated settings and ordering. The semantic output verifies
selection/hoisting; it does **not** prove material, tessellation, or export
behavior. Check a mesh output to exercise those stages.

Compare the selected tree with the requested subsystem: correct containers and
leaves, expected removals, no accidental whole-detector keep, and no unexpected
orphan drops. Original paths remain useful after hoisting. If output is empty or
too large, revisit observed names, the drop-all baseline, ancestry, and rule order
before tuning mesh quality.

When a GPU context is available, check visually with a one-shot render of the
geometry and config (`viewer shot` takes `-i`/`-c`, not a `.nhproj`):

```bash
nodehammer viewer shot -i detector.nhb -c scene.toml -o preview.png --shot-width 1920
```

Inspect the PNG for missing geometry, envelope solids hiding the detector,
unexpected opacity/colors, and excessive faceting. Do not start a blocking
viewer just to verify. If rendering is unavailable, report the validation and
conversion evidence and the outstanding visual check.

Deliver the editable config and any fragments, explain the selected parts,
merge/detail choices and unit assumptions, and give the command that reproduces
the output. Distinguish checks actually run from those still requiring the
user's geometry or backend.
