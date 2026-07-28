# Public API surface — sketch

Status: sketch, not a plan of record. Written while spiking the nanobind
bindings (`src/python/bindings.cpp`), to answer "what *is* the public API?"

## 1. Why this needs deciding at all

`include/nodehammer/**` is 69 headers with no public/internal distinction.
Everything is reachable, so every binding, every vendored slice, and every
future consumer silently pins whatever it touches. The spike hit this within
minutes: `DiagnosticList` has no `begin()`, so the binding uses `items()`,
which returns `const std::vector<Diagnostic>&` — the Python API is now coupled
to the diagnostics backing store being a `vector`.

## 2. Three consumers, and the strictest one wins

The surface is not one thing. Three consumers want it, under very different
constraints:

| Consumer | Link model | Language floor | Dependencies |
|---|---|---|---|
| Python module (`_nodehammer`) | static into one `.so` | C++23 | anything Conan resolves |
| CLI / viewer | static into an exe | C++23 | anything |
| **Amalgamated header** (`docs/event-display-design.md` §7) | compiled in the experiment's own TU | **C++17** connector / **C++20** convert | vendor flatbuffers; shim glm + unordered_dense; assume DD4hep/ROOT |

The amalgamation is the binding constraint, and it is *already* the most
carefully specified. §7.3 of the event-display design did the analysis: the
write slice uses glm only for column-major element access, and
`unordered_dense` only as `StrongId`-keyed scene maps. That analysis is
already a public-API boundary definition — it says exactly which types may
appear in the surface and how they may be used.

**Consequence:** define the core surface as what the amalgamation can carry,
then let each consumer add its own layer on top. A surface designed for the
Python bindings first would freely use C++23 and real glm, and would have to
be retrofitted later.

## 3. Layer 0 — vocabulary (shared by all three)

Types that appear in signatures. These are the ones that must survive the
glm/unordered_dense shims.

- **Semantic IR** — `SemanticScene`, `SemanticNode`, `SemanticLogicalVolume`,
  `SemanticShape` + `SemanticShapeVariant`, `SourceMaterial`, the `StrongId`
  family.
- **Render IR** — `RenderScene`, `RenderNode`, `MeshAsset`, `Vertex`,
  `RenderMaterial`.
- **Diagnostics** — `Diagnostic`, `DiagnosticSeverity`, `DiagnosticList`.
- **Config** — `NHConfig` (`config/config_ast.hpp`).

Rules that fall out of the amalgamation constraint:

- No glm *math* in any signature — element access only.
- Scene maps stay `StrongId`-keyed so the `std::unordered_map` alias shim holds.
- No `nlohmann::json` in the surface. It is a serialization detail; today
  `to_json` free functions sit right next to `Diagnostic`, which drags the
  dependency into the vocabulary header.

## 4. Layer 1 — pipeline verbs

One entry point per stage, each returning data plus diagnostics rather than
printing:

```
import      ImporterRegistry / ISemanticImporter -> ImportResult
config      ConfigLoader                          -> ConfigResult
select      SelectionEngine::prune
prepare     prepareSceneForTessellationFromInputs -> ScenePrepResult
tessellate  TessellationJob                       (cooperative)
build       buildSceneFromPaths                   -> SceneBuildResult
export      ExportConfig + exporter registry
serialize   toNhb(SemanticScene) -> bytes
```

`buildSceneFromPaths` and `toNhb` are the two the amalgamation needs; the
rest is the modular decomposition the CLI and viewer already use.

## 5. Layer 2 — per-consumer surfaces

### Python

```python
import nodehammer as nh

cfg    = nh.Config.from_file("odd.toml")        # or from_string(toml_text)
scene  = nh.import_geometry("odd.gdml")          # registry-resolved
scene  = nh.import_geometry(p, format="tgeo")    # explicit
nh.formats()                                     # -> ["gdml", "tgeo", "nhb", ...]

result = nh.build_scene(cfg, scene)              # -> RenderScene + diagnostics
nh.export(result.scene, "out.glb", unit_scale=0.01)
nhb    = nh.to_nhb(scene)                        # raw .nhb bytes

for d in result.diagnostics:
    print(d.severity, d.code, d.message)
```

The thing that makes bindings worth more than `subprocess`: **zero-copy mesh
access**. `MeshAsset` is already the right shape for it —
`std::vector<Vertex>` where `Vertex` is two tightly packed `glm::vec3`, plus
`std::vector<uint32_t>` indices. nanobind's `nb::ndarray` maps both without a
copy:

```python
mesh.positions   # (N, 3) float32 view, stride 24
mesh.normals     # (N, 3) float32 view, stride 24, offset 12
mesh.indices     # (M, 3) uint32 view
```

That is the actual argument for a Python API: geometry into numpy/trimesh/
pyvista without a serialization round-trip.

### Amalgamated connector (§7 of the event-display design)

```cpp
nh::SemanticScene scene = nh::importDD4hep(detector);
std::vector<uint8_t> nhb = nh::toNhb(scene);
nh::Viewer v{"localhost", 8080};
v.sendGeometry(nhb);
v.show(event);
```

Worth noting the symmetry: once the event API exists, the Python twin is
obvious and should be bound to the same core — `with nh.Viewer("localhost",
8080) as v: v.show(event)`.

### CLI

`nodehammer::cli::run(argc, argv)` — already extracted, already shared with
the Python console script.

## 6. Explicitly not public

- `viewer/**` — App, sokol, `ProjectFs`, `ZipWorkingSet`, archive export.
  Application internals; the wasm/native viewer is a *consumer* of the core,
  not part of its API.
- `detail/**` — already named as such.
- `lua/**` — a CLI front-end.
- `cli/**` except `run.hpp`.
- Tessellation internals (`tessellation_pass`, `primitive_tessellator`,
  `boolean_tessellator`) — the job and pipeline entry points are the surface.

## 7. Mechanism

The earlier instinct in this spike was per-symbol `NH_API`
visibility annotation. That is the wrong tool here:

- The Python module statically links the core into one `.so`. Measured: with
  the full pipeline bound, the module still exports exactly
  `PyInit__nodehammer`. Binding more API changes nothing about visibility.
- The amalgamation is compiled from source in the consumer's TU — §7 is
  explicit that there is "no binary boundary to protect".

So the surface needs to be **declared**, not *exported*. What actually needs a
machine-readable list is the amalgamator (§9.2) — it must know which headers
and TUs form the slice, and the golden-equivalence test (§10.2) enforces that
the assembled header matches the modular build. That list *is* the public API
definition, and it is load-bearing whether or not anyone annotates a symbol.

Visibility annotation becomes necessary only if the core ever becomes a shared
library with more than one consumer — the "one dylib for viewer + bindings"
option. Statically linking into both costs ~3 MB of duplicated disk and avoids
the entire question.

## 8. Prerequisite: stop printing

`build_scene` writes tessellation statistics to stdout. Fine for a CLI, wrong
for every other consumer — a Python caller or an in-process experiment client
wants that returned. Routing pipeline output through `DiagnosticList` /
`LogSink` instead of `std::print` is a precondition for any of this being a
real API, and it is a larger job than the packaging work around it.

## 9. Open questions

- Does `NHConfig` belong in the surface as a struct, or should the API take
  TOML text and keep the AST internal? The struct is large and churns.
- Semantic IR in the Python surface at all, or render-only? Semantic is where
  the selection predicates bite, so probably yes — but it doubles the
  vocabulary.
- `to_json` free functions currently live beside `Diagnostic`; splitting them
  out is what lets the vocabulary header stay nlohmann-free.
