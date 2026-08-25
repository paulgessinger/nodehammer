---
name: nodehammer
description: Drive the nodehammer CLI — convert HEP detector geometry (DD4hep XML, ROOT TGeo, .nhb) to glTF/OBJ/JSON, inspect a scene as JSON, validate and flatten TOML/Lua scene configs, pack and publish .nhproj project archives, and render or serve the 3D viewer. Use for nodehammer, nhb, nhr, .nhproj, nodehammer.toml, "convert geometry", "tessellate", "detector geometry", "wedge cut", NH#### diagnostic codes, or `uvx nodehammer`.
---

# nodehammer

A pipeline that imports detector geometry, applies a config that selects and
styles parts of it, and writes it out — as a mesh (glTF/OBJ), as an intermediate
scene, as a JSON report, or into an interactive viewer.

Same program either way: the compiled `nodehammer` executable and the
`nodehammer` command from the Python wheel are one implementation (`cli::run`
inside `libnodehammer`), so every command here behaves identically under
`uvx nodehammer`. Differences that do exist are noted as such.

## The output contract — read this before parsing anything

**stdout is the answer. stderr is everything about producing it.**

stdout carries the thing you would substitute into the next command: the
flattened document, the JSON report, the path to the archive that was written,
the URL being served. stderr carries diagnostics, progress and summaries.

```bash
archive=$(nodehammer project pack -c scene.toml -i det.root -o det.nhproj)
```

That yields the archive path, not a report about one. Two consequences:

- **Never parse stderr.** It is prose for a human and is not a stable format.
- **`-q` silences narration only.** Diagnostics, and the line a failing command
  prints before it exits non-zero, are never affected. A flag that hid errors
  would be a trap. `-v` restores narration; `-q`/`-v` may be typed before or
  after the subcommand.

When calling `nodehammer` from a script, quiet is already the default for the
library entry point; the executable defaults to narrating because a person typed
it.

## Commands

Five groups. Every one takes `-h`.

| | |
|---|---|
| `convert` | import → config → write. The **output extension decides how far the pipeline runs** |
| `inspect summary\|tree\|tags` | report on a geometry file; `--output-format json` |
| `config validate\|flatten` | check a config, or resolve it to one self-contained TOML |
| `project pack\|publish\|info` | build, publish and read `.nhproj` archives |
| `viewer open\|serve\|shot\|bench` | the 3D viewer, in four postures |

Option spellings are consistent across the tree: `-i/--input`, `-c/--config`,
`-o/--output`, `--input-format`, `--output-format`, `--color`, `--depth/-d`,
`--filter/-f` mean the same thing everywhere they appear.

### Commands that block — do not run these unprompted

| Command | What it does | Use instead |
|---|---|---|
| `viewer open` | opens a **native window and blocks** until the user closes it | `viewer shot -o out.png` |
| `viewer` (bare, with a path) | same — `open` is the default mode | `viewer shot` |
| `viewer serve` | starts an HTTP server, **opens a browser, and blocks** | `viewer serve --no-browser` in a background job, or `project publish` for static output |
| `viewer bench` | runs a GPU benchmark; needs a display | only when the user asked for numbers |

To *see* what a scene looks like, `nodehammer viewer shot <project> -o shot.png
--shot-width 1920` renders one PNG once the scene settles and quits. That is the
agent-appropriate way to check a geometry visually. It still needs a working GPU
context, so it will fail on a headless machine with no display.

Everything else in the tree runs to completion and exits.

### convert

```bash
nodehammer convert -i det.root -c scene.toml -o det.glb
nodehammer convert -i det.root -o scene.nhb          # stop at the semantic scene
nodehammer convert -i det.root -o a.glb -o b.obj     # several outputs, one import
```

The output extension picks the writer *and* the depth of the pipeline:

- `.nhb`, `.json` — the **semantic** scene. No tessellation. Fast.
- `.glb`/`.gltf`, `.obj`, `.nhr`, render-JSON — **tessellated** first.

`--timing` prints per-step wall clock, `--size-report` a payload breakdown; both
to stderr, and both survive `-q` because naming a report is asking for it.
`--strict` turns warnings into errors. `--wedge-cut START END` performs a real
Manifold boolean, removing the azimuthal sector from START to END degrees
measured from +x counter-clockwise — this is geometry, not a display effect.

### inspect — the machine-readable one

```bash
nodehammer inspect --output-format json summary -i det.root
nodehammer inspect --output-format json tree -i det.root --depth 3 --filter '*/Pixel*'
nodehammer inspect --output-format json tags -i det.root
```

**Text and JSON are not the same content in two skins.** The text view draws
tree lines, samples a tag key down to five values once it passes ten, and
appends a "(n shown, m filtered)" footer. Parse the JSON, which carries the
whole set and no drawing. Every document is stamped `"schema": 1`.

```jsonc
// summary
{ "schema": 1, "kind": "summary", "format": "tgeo",
  "nodes": 12043, "shapes": { "Box": 900, ... },
  "materials": ["Air", ...],                       // sorted
  "diagnostics": { "warnings": 3, "errors": 0 } }

// tree
{ "schema": 1, "kind": "tree", "shown": 40, "filtered": 12003,
  "maxDepth": 3,        // null when no limit applied — present, not absent
  "filter": "*/Pixel*", // null when none
  "nodes": [ { "path": "/World/Det", "name": "Det", "depth": 1,
               "children": 4, "leaf": false, "tags": {...} } ] }

// tags
{ "schema": 1, "kind": "tags", "nodeCount": 12043, "nodesWithTags": 800,
  "keys": { "subdetector": ["pixel", "strip"] } }   // every value, not a sample
```

`maxDepth` and `filter` are **present-and-null** rather than absent, so read them
unconditionally. `tags` is absent from a tree node that has none.

### config

Configs come in two spellings, and the file decides which: `.toml` (with an
`include` chain) or `.lua` (a script that is evaluated). Both commands take
either — **do not look for a separate Lua command, there isn't one**.

```bash
nodehammer config validate -c scene.lua
nodehammer config flatten  -c scene.toml -o flat.toml   # -o optional; stdout otherwise
```

`config flatten` is the tool to reach for whenever you need to know what a config
*actually* says: it resolves the include tree (or runs the script) and emits one
self-contained TOML that needs no companion files. Its output is round-trippable.

### project — `.nhproj` archives

A `.nhproj` is a ZIP holding a config, the geometry it names, and a manifest.

```bash
nodehammer project pack -c scene.toml -i det.root -o det.nhproj
nodehammer project info det.nhproj
nodehammer project publish det.nhproj -o site/     # static site, no server
```

**`nodehammer.toml` inside an archive is a reserved key.** It is the manifest
naming the entry config and geometry. `project pack` refuses rather than
overwriting when a config is actually named that, or an include resolves to it.
Do not hand-write it, and do not unzip an archive, edit, and re-zip — repack from
sources instead.

`project publish` produces a directory you can serve with any static file server
(GitHub Pages included). Prefer it over `viewer serve` for anything an agent
produces, because it terminates.

## Inherent limitations — not bugs to work around

**The importable set is smaller than the file types people assume.** Built in
unconditionally: `.nhb` / `.nhb.zst`, `.json` / `.json.zst`, and the synthetic
generator. Compile-time gated: `.root` (TGeo) and DD4hep XML. There is **no GDML
importer** — a `.gdml` path will not resolve however you spell `--input-format`.

**Heavy importers are compile-time options, not runtime ones.** `NODEHAMMER_WITH_TGEO`,
`NODEHAMMER_WITH_DD4HEP`, `NODEHAMMER_WITH_GEANT4`, `NODEHAMMER_WITH_GEOMODEL` are
**off by default**, and the released Python wheel is built without them. So
`cannot determine input format for 'det.root'` from a wheel install is the build
configuration, not a broken file and not a bug — the fix is a build with the
backend enabled, never a different flag. Check with
`nodehammer convert -i x.root -o /dev/null` and read the code, not the extension.

**DD4hep XML has no extension of its own.** It is recognised by sniffing for
`<lccdd` in the first 512 bytes, or by `--input-format dd4hep` when the sniff
fails. A generic `.xml` that is not DD4hep will not resolve.

**`viewer serve` needs a wasm runtime this build cannot produce.** It comes from
the separate `nodehammer-web` distribution, which the Python package depends on.
`NH1000` means it was not found; the command prints the search ladder it walked.
`--web-assets DIR` points at a locally built one.

**A wasm build has no `project` or `viewer serve`.** There is no host to serve
from — the wasm module *is* the viewer.

## Diagnostics

Every failure carries a stable `NH####` code, greppable and safe to match on.
Ranges: `NH00xx` config · `NH01xx` import · `NH02xx` scene ops · `NH03xx` TGeo ·
`NH04xx` selection · `NH05xx` tessellation · `NH06xx` export · `NH08xx` public
API · `NH09xx` CLI usage · `NH10xx` web runtime · `NH11xx` project archives.

The prefix in the code's *name* says how far it goes: fatal (the call could not
deliver), error (the result exists and part of it is wrong), warning (complete,
something was assumed).

| You see | It means | Do |
|---|---|---|
| `NH0101 cannot determine input format for '…'` | extension unknown **or backend not compiled in** | check the limitations above before anything else |
| `NH0900 positional path must be a .nhproj archive or a directory` | `viewer <path>` got something else | pass a `.nhproj` or a directory |
| `NH0901 path not found` / `input file not found` | a typo, before any window opens | — |
| `NH1000` | wasm runtime missing | `pip install nodehammer-web`, or `--web-assets DIR` |
| `NH1001` | runtime and library disagree | rebuild both halves together |
| `NH1100` | archive could not be assembled or read | `project info` on it; do not unzip by hand |
| `NH0008 drop_coincident_faces … without merge_descendants` | the option is silently a no-op | enable `merge_descendants` in the same scope |

## Recipes

**"What is in this file?"** → `nodehammer inspect --output-format json summary -i FILE`

**"What does this config actually do?"** → `nodehammer config flatten -c FILE`
(resolves includes and Lua), then read the TOML.

**"Show me the geometry."** → `nodehammer viewer shot FILE -o /tmp/shot.png
--shot-width 1920`, then look at the PNG. Never `viewer open`.

**"Make something I can share."** → `project pack` then `project publish -o site/`.

**"Is this config valid?"** → `nodehammer config validate -c FILE`; exit code is
the verdict, the report is on stderr.

**"Convert for a web page."** → `-o out.glb` (glTF binary), not `.obj`.

## Do not

- **Do not run `viewer open`, bare `viewer <path>`, or `viewer serve` without
  being asked.** They block until a human intervenes.
- **Do not parse stderr**, or the text form of `inspect`. Use
  `--output-format json`.
- **Do not unzip, edit and re-zip a `.nhproj`.** Repack from sources.
- **Do not hand-write `nodehammer.toml` inside an archive** — `project pack`
  owns that key.
- **Do not treat a missing importer as a broken file.** Check the compile-time
  gates first.
- **Do not add `-q` to make output parseable.** It only removes narration from
  stderr; stdout was already clean.

## Getting this skill

It ships inside the binary and is installed with:

```bash
nodehammer skills install            # into ~/.agents/skills, linked from ~/.claude
nodehammer skills install --scope project
nodehammer skills list
```
