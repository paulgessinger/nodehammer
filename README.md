# nodehammer

A C++23 pipeline for converting high-energy-physics detector geometry into
render-ready meshes and serialized semantic scenes, plus an interactive
GPU/web viewer for inspecting the result.

nodehammer imports detector geometry from formats like DD4hep, TGeo, and
native serialized scenes, lowers it through a two-stage intermediate
representation, and exports either GPU-ready meshes (glTF/GLB/OBJ) or
source-faithful semantic scenes (JSON/FlatBuffers). A TOML config drives node
selection, shape deduplication, and tessellation.

## The two halves

nodehammer is two cooperating parts:

1. **Conversion pipeline** — `src/{config,ir,tessellation,selection,cli}`. The
   `nodehammer` CLI: import geometry → lower semantic → render IR → export.
2. **GPU / web viewer** — `src/viewer` + `src/web`. A [sokol_gfx][sokol]-based
   renderer (Metal / OpenGL / WebGPU backends), Dear ImGui UI, IBL/AO render
   passes, and PNG export. The WASM build serializes the semantic scene to
   `.nhb`, ships it to a Web Worker, tessellates there, and renders the baked
   `.nhr` geometry.

The viewer is off by default; the pipeline builds and tests without it.

## The two intermediate representations

- **Semantic IR** (`include/nodehammer/ir/semantic.hpp`) — the source-faithful
  scene graph. Nodes reference logical volumes, which reference shapes +
  materials; transforms are double precision. This is what importers produce
  and what JSON / `.nhb` FlatBuffers serialize.
- **Render IR** (`include/nodehammer/ir/render.hpp`) — GPU-ready output. Nodes
  reference shared, immutable mesh assets (interleaved position+normal
  vertices, triangle indices) and materials. The tessellation pass produces it
  from the Semantic IR.

## Pipeline stages

The canonical flow lives in `src/cli/cmd_convert.cpp`:

1. **Config load + validate** (`src/config`)
2. **Import** — an `ISemanticImporter` resolved by format name or file
   extension (`src/ir/<format>/semantic/importer.cpp`). Import is
   partial-success: a scene may be produced even alongside diagnostics.
3. **Select** — keep/drop `[[selection_rules]]` (`src/selection`)
4. **Deduplicate** — value-based merge of materials/shapes/logical volumes
5. **Tessellate** — Semantic → Render IR; primitives via
   `primitive_tessellator`, boolean/CSG via `boolean_tessellator` (backed by
   the [manifold][manifold] library)
6. **Export** — an `IRenderExporter` (glTF/GLB/OBJ) or `ISemanticExporter`
   (JSON / `.nhb` FlatBuffers), resolved by output extension/format

Every stage returns a `DiagnosticList` rather than throwing; `--strict`
promotes warnings to failures.

## CLI

A single `nodehammer` binary (`src/cli/main.cpp`), CLI11-based. Subcommands:

| Command | Purpose |
|---|---|
| `convert` | Import → tessellate → export render geometry |
| `dump-semantic` | Import → export the semantic scene (JSON / `.nhb`) |
| `dump-render` | Import → tessellate → dump the render scene |
| `inspect` | Summarize a geometry file |
| `validate-config` | Parse and validate a TOML config |
| `config-flatten` | Resolve a config to a single canonical TOML |
| `viewer` | Launch the interactive viewer (requires `NODEHAMMER_WITH_VIEWER`) |

## Build

All dependencies are managed with [Conan][conan]; the build is driven through a
[`Justfile`](Justfile). A C++23 toolchain is required (the CI/dev toolchain is
GCC 15).

```bash
just deps        # conan install (add -o '&:viewer=True' for the viewer)
just configure   # cmake configure via the conan preset
just build       # build the library, CLI, and tests
just test        # ctest
```

The heavy importer backends are **off by default** and gated behind CMake
options (and matching preprocessor macros):

| CMake option | Macro | Enables |
|---|---|---|
| `NODEHAMMER_WITH_TGEO` | `NH_WITH_TGEO` | TGeo/ROOT importer |
| `NODEHAMMER_WITH_DD4HEP` | `NH_WITH_DD4HEP` | DD4hep importer |
| `NODEHAMMER_WITH_VIEWER` | `NH_WITH_VIEWER` | sokol/Dear ImGui viewer |

Tests for a gated backend are only compiled when its option is on. FlatBuffers
headers are code-generated from `schemas/semantic.fbs` via `flatc` at build
time.

(`NODEHAMMER_WITH_GEANT4` also exists and links Geant4, but the GDML importer
behind it is not yet wired into the registry.)

## Repository layout

```
include/nodehammer/     public headers (mirrors src/ structure)
src/
  cli/                  nodehammer binary + subcommands
  config/               TOML → NHConfig AST, validation, predicate parser
  selection/            selection-rule engine + glob predicate matcher
  ir/                   the two IRs and their format adapters:
    <format>/<ir-kind>/{importer,exporter}.cpp
    e.g. gltf/render/exporter.cpp, dd4hep/semantic/importer.cpp, fb/semantic/…
  tessellation/         Semantic → Render IR lowering
  viewer/               interactive GPU viewer (gated)
  web/                  WASM/emscripten entry points
docs/                   design notes (predicate grammar, PNG export, …)
fixtures/               example configs and importer test fixtures
schemas/                FlatBuffers schema (.fbs)
recipes/                vendored Conan recipes (manifold, sokol-shdc)
```

Each format adapter follows the convention
`src/ir/<format>/<ir-kind>/{importer,exporter}.cpp` with the matching header
under `include/nodehammer/ir/<format>/<ir-kind>/`. Registries
(`src/ir/semantic/importer_registry.cpp`, `src/ir/{semantic,render}/…`) wire the
built-in adapters together via `makeDefault()`.

## Config

TOML ([tomlplusplus][toml]) parses to an `NHConfig` AST (`config/config_ast.hpp`).
Node-matching predicates accept both a verbose table form and a concise
expression string (e.g. `tag.sensitive == "true" && path ~= "**/Pixels/**"`);
both parse to the same AST. See [`docs/predicate-expressions.md`](docs/predicate-expressions.md)
for the grammar. Example configs live under `fixtures/configs/`.

## Style

`clang-format` and `clang-tidy` are enforced (`.clang-format`, `.clang-tidy`,
pre-commit hooks). Two rules the linters don't fully catch, from
[`STYLE.md`](STYLE.md):

- Always brace the body of `if`/`else`/`for`/`while`/`do-while`.
- Avoid bare `operator[]` on vectors/maps; prefer range-`for`, iterators, or
  bounds-checked `.at(i)`.

[sokol]: https://github.com/floooh/sokol
[manifold]: https://github.com/elalish/manifold
[conan]: https://conan.io
[toml]: https://github.com/marzer/tomlplusplus
