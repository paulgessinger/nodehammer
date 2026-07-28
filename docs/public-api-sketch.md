# Public API surface — sketch

Status: §3.1, §4 and §7 are now plan of record and partly built — see
§11 for what landed and what changed. The rest remains sketch. Written while
spiking the nanobind bindings (`src/python/bindings.cpp`), to answer "what *is*
the public API?"

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
| **Amalgamated header** (`docs/event-display-design.md` §7) | compiled in the experiment's own TU | **C++20** (single floor) | vendor flatbuffers; shim glm + unordered_dense; assume DD4hep/ROOT |

The amalgamation is the binding constraint **for the semantic side only** —
and it is narrower than this section originally claimed. §7.1 of the
event-display design scopes the slice to "the semantic IR, the serializer, and
the selected importer" plus the connector; `buildSceneFromPaths`, `RenderScene`,
`MeshAsset` and tessellation appear nowhere in it. So `SemanticScene` must
survive the glm/unordered_dense shims and the C++20 floor, while the render
vocabulary answers only to Python, the viewer and the CLI.

Within that scope it is *already* the most carefully specified. §7.3 of the event-display design did the analysis: the
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

### 3.1 Opaque handles: hold the IR, don't see it

Consumers need to *hold* an IR between calls — import once, then select,
tessellate, export, inspect — without the round-trip through bytes that a
process boundary would force. They do not need to see its layout.

So the vocabulary types enter the surface as **opaque handles**: forward
declarations plus accessor functions, never field access. Python holds an
`nh.SemanticScene`, passes it to the next verb, and keeps it alive; it never
learns that `nodes` is an `ankerl::unordered_dense::map`.

What this buys:

- `SemanticScene`'s four `StrongId`-keyed maps
  (`ir/semantic.hpp:247-251`) can change container, gain fields, or reorder
  without breaking a single consumer.
- The amalgamation's `unordered_dense` shim (§7.3) still has to make the
  *internals* compile, but the shim can no longer leak into a signature — so
  "byte-identical output not required" stops being a risk to the API.
- It matches the §7.4 usage already sketched for client code: hold a
  `nh::SemanticScene` between `importDD4hep` and `toNhb`, poke at nothing.

The spike already follows this by accident: `RenderScene` is bound with
computed properties (`node_count`, `mesh_count`, `material_count`) and no
field access at all.

**Lifetime needs to be made consistent.** `SceneBuildResult::scene` is a
`std::shared_ptr<RenderScene>` — refcounted, and nanobind's `shared_ptr`
caster maps it directly. `ImportResult::scene` is a `SemanticScene` by value.
Handles want one ownership story; shared ownership is the one that survives a
Python object outliving the call that produced it.

### 3.2 The one deliberate exception: vertex buffers

Zero-copy mesh access does expose binary layout — a strided ndarray bakes in
`sizeof(Vertex) == 24` and the position/normal offsets. That is fine, because
this particular layout is *already* a frozen public contract. `schemas/render.fbs:24-26`:

> Interleaved vertex: matches `nodehammer::Vertex` (pos+normal, 6 contiguous
> floats) so the codec can memcpy whole arrays on the hot path.

The FlatBuffer codec memcpys whole arrays against that promise, and it is
versioned with the schema. Handing out an ndarray view over the same bytes
commits to nothing that `.nhr` files don't already commit to.

So the line is: **scene structure opaque, vertex buffers contractual.** Those
are the two different stability guarantees, and they should be documented as
such rather than blurred.

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

`importDD4hep`/`importTGeo` and `toNhb` are the ones the amalgamation needs —
not `buildSceneFromPaths`, which an earlier draft of this doc claimed. The rest
is the modular decomposition the CLI and viewer already use.

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

## 7. Mechanism: `NH_API`

The surface is marked in the code, per symbol, with an `NH_API` macro. The
alternative considered — a separate manifest of "public headers" — was
rejected because a list in a doc drifts away from the code it describes,
while a macro sitting on the declaration cannot.

Note what the macro does *not* do today. Neither current consumer has a
binary boundary:

- The Python module statically links the core into one `.so`. Measured with
  the full pipeline bound: the module exports exactly `PyInit__nodehammer`,
  and binding more API does not change that.
- The amalgamation compiles in the experiment's own TU —
  `docs/event-display-design.md` §7 is explicit that there is "no binary
  boundary to protect".

So `NH_API` starts as a **declaration** that happens to already be the right
**export** mechanism the day a shared core appears. That day is plausible:
a native viewer and a Python module both wanting the same code is exactly
the "one dylib" shape. Statically linking into both remains an option — it
costs ~3 MB of duplicated disk and sidesteps ABI entirely — but the
annotation is what keeps that a free choice rather than a forced one.

### 7.1 Four branches, not two

```cpp
#if defined(NH_AMALGAMATED)
#  define NH_API                                     // one TU, no boundary (§7)
#elif defined(__EMSCRIPTEN__)
#  define NH_API                                     // exports via -sEXPORTED_FUNCTIONS
#elif defined(_WIN32)
#  define NH_API __declspec(dllexport)               // dllimport for consumers
#else
#  define NH_API __attribute__((visibility("default")))
#endif
```

The amalgamation branch is the one that is easy to forget, and the one that
breaks the §7 vendoring story if it is wrong.

### 7.2 Annotation nobody links against is annotation nobody can trust

The failure mode of adopting `NH_API` early is annotating it wrong for two
years, then discovering every mistake at once when the shared build finally
happens — paying the cost without the payoff.

So pair the macro with a **CI-only shared build**: compile `nodehammer_lib`
as `SHARED` with `-fvisibility=hidden`, link a small consumer that exercises
the public API, and let missing annotations fail as undefined symbols. The
artifact is never shipped; it exists to keep the annotation honest from the
first commit. One extra CMake configuration.

That check is also the only thing that catches the failure modes the macro
alone will not:

- **Vtable emission** follows the key function's visibility; a polymorphic
  type exported without its key function is a runtime surprise, not a link
  error. `ISemanticImporter` is the live example.
- **Templates** instantiated on both sides can end up as distinct hidden
  instantiations, breaking `type_info` identity and function-pointer
  comparison.
- **Inline functions** in the surface — of which the vocabulary headers have
  many.

Current risk is low and worth recording: the only exception types thrown are
`std::runtime_error` (21 sites) and `std::logic_error`, with **no custom
exception classes**. Standard exception typeinfo comes from the C++ runtime
at default visibility, so the classic "hidden typeinfo makes `catch` silently
miss and `terminate()` fire" trap is not live. It becomes live the day
someone writes `class ConfigError : public std::runtime_error`.

### 7.3 What `NH_API` still does not cover

The macro marks *symbols*. The amalgamator (§9.2) needs *files* — which
headers and TUs form the slice, with the golden-equivalence test (§10.2)
enforcing that the assembled header matches the modular build. Related, but
not the same list.

They are complementary rather than duplicative: the set of files containing
`NH_API` symbols is a good seed for the amalgamator's manifest, and a file in
the slice with no annotated symbols in it is a smell worth flagging.

Two further constraints sit outside the macro's reach and need their own CI,
already anticipated by §10.3: the C++17/C++20 language floor for the slice,
and the vendor/shim/assume dependency discipline of §7.3.

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
- ~~Ownership has to be settled before anything is annotated~~ — settled, see
  §11.

## 10. Suggested order

Annotation is cheap per symbol and expensive to redo, so settle the shape
first and mark second.

1. ~~**Ownership + opaque-handle accessors** (§3.1)~~ — done, §11.
2. **`NH_API` macro + the CI shared build** (§7.1, §7.2) — mechanism and its
   enforcement land together, so the first annotation is validated.
3. **Annotate the uncontroversial vocabulary**: `Diagnostic`,
   `DiagnosticList`, the `StrongId` family, `RenderScene`/`MeshAsset`
   accessors.
4. **Stop printing** (§8) — the precondition for any non-CLI consumer.
5. **Annotate the pipeline verbs** (§4) once §9's open calls are settled.
6. **Amalgamator manifest** (§7.3), seeded from the annotated set.


## 11. What was decided, and what it cost

Step 1 of §10 is built. The decisions, and the two places reality differed from
the sketch above.

### The handle is a value outside and a `shared_ptr` inside

`nodehammer::SemanticScene` and `nodehammer::RenderScene` are value types that
copy cheaply, can be held across calls, and expose no fields. That satisfies
§7.4 of the event-display design — `SemanticScene scene = importDD4hep(d);`
still compiles and still means what it says — while giving Python a refcount
that outlives the call that produced it. §3.1 asked for "one ownership story";
this is it, and it needed no change to how the pipeline passes scenes around.

**Handles are read-only, and that is load-bearing rather than cautious.**
`build_pipeline.hpp` invariant #5 says prep copies its inputs so the caller's
pristine scene is never mutated — which is what the wedge re-aim path rebuilds
from. A handle that shared a *mutable* scene would break that with no compile
error. So mutation stays on the internal struct and public verbs return new
handles.

Two supporting decisions that were not obvious up front:

- **`SceneBuildResult::scene` had to become `shared_ptr<const>`.** It was the
  only non-const `shared_ptr<RenderScene>` in the tree. Wrapping it by implicit
  conversion would have left the producer holding a mutable alias to the same
  object — and mutating through it would dangle the `string_view`s and `span`s
  the accessors hand out, not merely change values.
- **Derived data is computed eagerly at wrap time**, into a state object the
  handle owns. Map order is not a stable public order — it varies with the
  container, the standard library and the amalgamation's `unordered_dense`
  shim — so the handle publishes DFS-preorder node ids and ascending ids for
  everything else. Eager rather than lazy because scenes cross thread
  boundaries (`build_controller.cpp` hands one to a worker), and computing once
  up front removes the thread-safety question instead of answering it.

### Namespaces: `nodehammer::` is the API

No `nh` alias, in either the modular or the amalgamated header. Internals move
to `nodehammer::detail` as the API claims their names — the convention already
existed (`include/nodehammer/detail`, and `nodehammer::viewer` carving out the
viewer), it was just applied unevenly.

Applied narrowly on purpose. `SemanticScene`, `RenderScene`, `RenderNode`,
`MeshAsset` and `ImportResult` moved; `SemanticNode`, `SemanticShapeVariant`
and the thirteen shape structs did **not**, because the surface names them
through `*View` types and requalifying ~750 call sites would buy tidiness and
nothing else. The render side went further than the semantic side for a
specific reason: `RenderNode::extras` is an `nlohmann::json` alias that had to
be put out of reach of a public signature.

### `ShapeKind`, and why the variant stays hidden

The surface exposes the shape *kind* as a flat enum and no parameters. That is
what let both near-identical thirteen-way `std::visit` blocks
(`cmd_inspect.cpp`, `cmd_dump_semantic.cpp`) be deleted: they existed only
because there was no way to ask "what kind of solid is this". The mapping is
the variant index, pinned by `static_assert`s on `variant_size` and on the
boundary alternatives, so adding or reordering one breaks the build instead of
silently reporting the wrong kind.

### The C++20 floor is enforced, not asserted

§7.5 of the event-display design says the slice must stay within its floor
"forever", but nothing checked it: the repo compiles at C++23 and CI asserts
`<print>` exists. `nodehammer_cxx20_floor` recompiles the API sources and
everything they include at C++20. It is never linked or shipped; it exists to
fail, and it was verified to fail (a `std::print` call breaks it).

This is the same argument §7.2 makes for pairing `NH_API` with a shared build,
arrived at independently — an unenforced rule about a build nobody performs is
a rule that is already broken.

### What the migration proved, and what it did not

`inspect` and `dump-semantic` now read scenes only through the handle. Output
is byte-identical to the pre-migration binary across the ODD geometry — the
full 325k-line tree, plus depth-limited and filtered variants — with one
intended exception: `inspect summary` lists materials in id order rather than
map order, which is the handle declining to expose an order that was never
stable.

One gap the migration surfaced: `dump-semantic` has its own display names
(`bool/union`, not `union`; the unknown type carried inline), so `kindName()`
is not universally a drop-in. `kind()` still expresses it without the variant.

### Where the lifetime evidence is strong, and where it is not

The C++ side is well covered: the full suite (404 cases) is clean under an ASan
build, and the handle tests assert pointer identity against the internal vector
and that both views and `ownedVertices()` outlive every handle.

The Python `ndarray` owner chain is **not** empirically verified, and it is
worth being precise about why. ASan cannot intercept on that path — the
interpreter is not itself instrumented, so preloading the runtime via
`DYLD_INSERT_LIBRARIES` reports "interceptors are not working". Nor could a
negative control be built without it: removing the `owner` argument and then
dropping the scene still reads correct data, even after forcing 64 MB of
allocation churn, because freed-but-unreused memory reads the same as live
memory.

So the owner chain rests on construction (`nb::object self` is passed as the
array's owner, and the `Mesh` object holds a refcount on the scene) and on the
C++ layer beneath it — not on a demonstrated failure. Anyone changing those
accessors to take `const MeshView &` instead of `nb::object self` would silently
break it, and no test in this repo would notice. That is a real gap, and the
fix is a sanitizer-instrumented Python, not a cleverer assertion.

### Still true, and still the biggest item

§8. `build_scene` prints tessellation statistics to stdout, and that is now
visible on the Python path — the binding test prints them mid-notebook. It
remains a precondition for any non-CLI consumer and is larger than the
packaging work around it.
