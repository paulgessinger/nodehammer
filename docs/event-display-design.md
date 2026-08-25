# Event display — design & integration strategy

Status: **design / not yet implemented.** This document captures the target
architecture for adding a per-event display to nodehammer and, more importantly,
for *driving it from experiment frameworks* (DD4hep / TGeo / GeoModel / edm4hep)
that have heavy, pinned runtime environments and are reluctant to add software to
their dependency build stack.

It is the product of a design discussion; nothing here is built yet. Where a
claim depends on the current code, the relevant file is linked so the assumption
can be re-checked before implementation.

**Main path in one line:** a per-event JSON POST loop into a viewer server, plus
a single **amalgamated C++ header** (compiled in the experiment's own
environment) for the cases that need in-process geometry/event conversion.
Everything heavier — a C API, a prebuilt client `.so`, Arrow, GDML — is deferred
to §12 and is explicitly *not* on the critical path.

---

## 1. The problem and the binding constraint

We want to render individual physics events (hits, tracks, calorimeter deposits,
vertices, jets, MET) on top of the detector geometry nodehammer already imports.
Event data will come from `edm4hep`, possibly an Arrow columnar form, and other
sources.

The hard part is **not** rendering — it is getting event (and sometimes
geometry) data *out of* an experiment framework and *into* the viewer. Two
constraints dominate every decision:

1. **Experiments resist adding software to their dependency build stack.** A new
   spack/LCG package, a `find_package`, a version to provision — all high
   barriers. Copying a single file into their tree, running a CLI tool, or
   POSTing to an HTTP endpoint are low barriers.

2. **The real technical blocker is environment coupling, not just ABI.** HEP
   frameworks pin specific (often old) compilers and libstdc++ versions. Worse,
   **DD4hep consults the runtime environment to construct geometry** (plugin
   service, ROOT dictionaries, XML that names shared libraries resolved via
   `LD_LIBRARY_PATH`). A statically-linked, environment-free binary can therefore
   *never* perform DD4hep conversion — no amount of linking fixes it; the
   conversion has to run where the environment is.

### 1.1 Two principles that fall out

- **Separate the build-time surface from the runtime surface.** The build-time
  surface (anything in their CMake / link line / package manager) must be *at
  most one vendorable header, ideally nothing*. Everything heavier — a CLI on
  `PATH`, an HTTP endpoint — lives on the runtime surface, which experiments
  tolerate.

- **The wire is the contract; there is no C ABI on the main path.** Producer and
  consumer exchange serialized data over HTTP (JSON, or FlatBuffer bytes),
  never C++ or C types across a binary boundary. The stable, language-neutral
  contract is **HTTP + the event/geometry schema**, not a function signature.
  The client-side C++ helper is a convenience compiled *from source in the
  experiment's own TU*, so it can be idiomatic C++17/20 — it protects no binary
  boundary and needs no `extern "C"`.

---

## 2. Where the code is today (grounding)

- **Viewer** (`src/viewer/`) is a native + wasm app (sokol_gfx / Dear ImGui). It
  loads a scene through `ProjectFs` → `BuildSession` → `SceneBuildJob`
  (`src/viewer/build_session.cpp`) and **only ever imports the FlatBuffer
  semantic format**. On web it is an HTTP *client* (`emscripten_fetch` in
  `url_project_fs_web.cpp`). **There is no server, socket, or websocket code
  anywhere.**
- **Two IRs**: semantic (`src/ir/semantic.hpp`) and render
  (`src/ir/render.hpp`). Serialized via FlatBuffers —
  `schemas/semantic.fbs` (`file_identifier "NHS8"`, `.nhb`) and
  `schemas/render.fbs` (`"NHR8"`, `.nhr`). `.nhb.zst` is a zstd frame.
- **Importer registry** (`src/ir/semantic/importer_registry.cpp`,
  `makeDefault()`) resolves adapters by format/extension; heavy backends
  (`NH_WITH_TGEO`, `NH_WITH_DD4HEP`, Geant4/GDML) are compile-gated.
- **Selection** (`SelectionEngine::prune()`) runs `[[selection_rules]]` *before*
  tessellation. Config is TOML → `NHConfig`, validated by `config validate`,
  with a predicate-expression grammar (`docs/predicate-expressions.md`).
- **GeoModel support is greenfield** — the old `NODEHAMMER_WITH_GEOMODEL` flag
  was a no-op and was removed. There is no GeoModel importer.
- **No explicit units or coordinate-frame tag** exists in the semantic IR
  (implicit mm/cm by source system). This is a latent trap for overlays; see §3.

---

## 3. The Event IR, schema, and wire format

A third IR, sibling to semantic/render, plus a serialization format:

- **`EventScene`** — plain C++ structs (like the other IRs), holding collections
  of event objects: point clouds (hits), polylines (tracks/trajectories),
  boxes/towers with energy (calo), points (vertices), cones (jets), vectors
  (MET), plus per-object attributes (energy, pt, pid, time, collection name).
- **`schemas/event.fbs`** — its own root table, `file_identifier "NHE8"`,
  extension `.nhe`, reusing the `.zst` convention. Columnar layout (parallel
  numeric arrays per collection).
- **Adapters** slot into the existing convention: `ir/<format>/event/{importer,
  exporter}.cpp` with a new `makeDefault()`.

### 3.1 Contract requirements (non-negotiable in the schema)

Because event data is produced by a *different* system than the geometry, the
schema must pin what the geometry IR leaves implicit:

- **`length_unit`, `energy_unit`, `momentum_unit`** — explicit. Avoids the
  mm-vs-cm / MeV-vs-GeV class of bugs.
- **Handedness / coordinate frame** — explicit; overlays align to the geometry's
  world-transform space (`SemanticNode::worldTransform`, a `glm::dmat4`).
- **Geometry reference** — content hash of the `.nhb` (reproducible) with a
  logical name/version fallback. Events reference geometry; they never carry it.
- **Schema/protocol version** — so a stale client fails loudly (see below).

### 3.2 One schema, two encodings — JSON is the default

The event data has **one schema** (`event.fbs`) with **two wire encodings**, so
we never maintain two definitions that can drift:

- **JSON — the default, primary encoding.** FlatBuffers' own parser can read and
  write JSON *against the `.fbs` schema* (flatc / the runtime parser + reflection).
  So the JSON shape *is* the schema's projection: the server parses incoming JSON
  against the same `event.fbs` and lands in the same object, schema-validated for
  free. No hand-maintained second schema.
- **Binary FlatBuffer — a supported optional encoding.** Same schema, compact and
  zero-copy. Mainly for the convert client (which already has flatbuffers
  vendored, §7) and for pathologically large events.

The server accepts **both**, dispatched by `Content-Type` (`application/json`
vs `application/octet-stream`), via the importer registry. On the server both
nlohmann and flatbuffers are trivially present, so supporting both costs almost
nothing.

**Why JSON as default** (not FB):

- **Openness.** Any language posts events with no toolchain — Python, Julia,
  Rust, JS, even `curl`. FB would require flatc + generated code per language.
  The wire protocol *is* the cross-language binding.
- **Slim client stays slim.** The connector-only client (§7.1) does not vendor
  flatbuffers (that is behind the convert gate). Emitting event JSON needs only a
  **~40-line hand-rolled writer** — no nlohmann on the client. Robust JSON
  *parsing* (nlohmann) lives **server-side only**. Writer on the client, parser
  on the server: each side uses the right tool.
- **Debuggable / testable.** JSON fixtures are diffable and hand-authorable — a
  real asset for the conformance kit (§10.4) and unit tests. FB fixtures are
  opaque blobs.
- **Size mostly doesn't matter here.** Event display is human-paced (one event at
  a time), and HTTP gzip compresses JSON ~10×, so FB's size/parse advantage only
  bites on very large events (heavy-ion, full-calorimeter dumps) — exactly where
  the optional binary encoding is the escape hatch.

Two JSON disciplines: print doubles with full precision (`%.17g`, or positions
drift against the geometry), and use **NDJSON** (one event per line) for
multi-event *files* — a natural, stream-friendly container that binary FB would
need its own framing for.

### 3.3 Decouple geometry from events

The viewer loads geometry once; events reference it and carry only per-event
objects. Payloads stay tiny, and producers never ship geometry with each event.

---

## 4. Transports and the driving model

### 4.1 Blocking POST — the "next event" loop

WebSocket is unnecessary. The interaction is a synchronous HTTP long-poll, and
the client-side helper is idiomatic C++ (compiled in the experiment's env):

```cpp
nh::Viewer v{"localhost", 8080};              // RAII — closes on scope exit
for (const auto& evt : events) {
    nh::Event e{evt.run, evt.id};
    e.addHits("PixelHits", evt.hits);         // std::span<const double>
    for (const auto& t : evt.tracks) { e.addTrack("Tracks", t.pts); }
    if (v.show(e) == nh::Action::Quit) { break; }   // enum class
}
```

`v.show(e)` does `POST /event` and the **server holds the response open** until
the viewer user advances. It then returns `Next` / `Quit` (`Prev` optional if the
producer is random-access). One request per event, single connection,
human-paced. If the viewer closes, the socket drops and the call returns `Quit`.

The **server is a rendezvous**: it pairs a producer's held `POST /event` with an
attached viewer and releases the producer when the viewer signals advance. That
indirection is what makes the *same producer code* drive either viewer type.

Caveat: a long-poll can be held for minutes. Fine on localhost / SSH tunnels
(TCP keepalive); if a proxy is ever interposed, send periodic chunked whitespace
to defeat idle timeouts.

### 4.2 File transport

Producer writes `.nhe` / `.json` / NDJSON; the viewer loads it (or watches a
directory). This is the offline-scanning and reproducible path, and it shares all
parsing with the POST path.

### 4.3 Native and web from one server

Two attach modes, one rendezvous, mediated identically:

- **`nodehammer view`** — native sokol window + HTTP server embedded in-process.
  Producer POSTs → in-process queue → native render → user advances →
  in-process condvar releases the held POST.
- **`nodehammer serve`** — **headless**: serves the bundled wasm viewer + the
  geometry `.nhb` + the event endpoints over HTTP. The browser is the renderer.

The web receive path reuses the existing `emscripten_fetch` GET client: the
browser long-polls `GET /wait-event` (symmetric with the producer's long-poll
POST); on advance it hits `POST /advance`, which releases the producer.

Consequence worth stating: **`serve` needs no GPU/display on the server** —
rendering happens in the browser. This is what makes headless batch nodes /
lxplus work at all, so serving the web build is load-bearing, not a convenience.

### 4.4 Endpoint sketch

| Method / path       | Purpose                                             |
|---------------------|-----------------------------------------------------|
| `POST /geometry`    | Push geometry (source recipe, `.db` bytes, or `.nhb`) |
| `POST /config`      | Push a TOML selection/style config                  |
| `POST /event`       | Push one event (JSON or FB); **blocks** until advance |
| `GET  /wait-event`  | (web viewer) long-poll for the next event           |
| `POST /advance`     | (web viewer) signal next / quit                      |
| `GET  /` , assets   | (serve mode) the bundled wasm viewer + `.nhb`       |

---

## 5. Geometry ingestion

Conversion stays where a proper build (or the right environment) exists; the
producer only ever transports something it can already emit. Which side runs the
heavy import depends on the source and the server flavor (§6).

- **DD4hep / TGeo, reproducible from source.** Geometry is reproducible from its
  recipe, so the producer need not marshal live objects: `POST /geometry` with a
  compact-XML path and the (DD4hep-linked) server re-imports in its own process
  where the environment resolves. Lightest path — just a string.
- **GeoModel (ATLAS).** GeoModel's canonical persistency format *is* a SQLite
  `.db` (written by `GeoModelWrite`/`GMDBManager`, read by `GeoModelRead`) — a
  legitimate, self-contained, portable serialization. Producer either points at
  an existing `.db` (zero code) or, for a live in-memory tree, serializes it with
  their *already-present* GeoModelIO (`GeoModelWrite` to a temp / `:memory:`
  `.db`) and POSTs the bytes. Server side needs a **new gated GeoModel importer**
  (`ir/geomodel/semantic/importer.cpp`, `GeoModelRead` → `GeoPhysVol` → semantic
  IR). Caveats: size (use zstd / path-passing), and GeoModel schema-version
  coupling between the producer's writer and the server's reader (pin it,
  diagnose mismatch).
- **Live, runtime-built geometry** (alignment applied, conditions-dependent,
  procedurally generated) — the case with no file to point at. This is where
  client-side conversion via the amalgamated header (§7) earns its place;
  otherwise prefer the recipe path above.

### 5.1 Metadata is load-bearing

Any importer must populate `SemanticNode::originalPath` and lift meaningful
`tags` (subdetector, sensitive, …). Filtering (§8) is only as good as the
metadata surfaced. For GeoModel this is a first-class importer output, not an
afterthought.

---

## 6. Deployment patterns

The client protocol is **identical across server flavors**; how a server got its
conversion capability is invisible to the producer.

### 6.1 Server flavors (build modes of one codebase)

| Flavor | Build | Converts | Runs where | Use |
|---|---|---|---|---|
| **Static CLI** | env-free deps static (`-static-libstdc++`, glibc dynamic) | light formats + pre-made `.nhb`/`.nhe` | anywhere, no env | laptop viewer, offline scanning |
| **DD4hep/TGeo-linked** | heavy backends on, run inside a resolving env | full DD4hep/TGeo import from source | lxplus / a spack view | online / integration |
| **GeoModel-linked** | + GeoModel importer | `.db` import | ATLAS env | ATLAS event display |

"Static" and "heavy" are just presets of the same tool. The static flavor is for
environment-free installs; the heavy flavors run *inside* the experiment's
environment so DD4hep/GeoModel resolve.

### 6.2 Topologies

- **Local.** `nodehammer view` on a workstation; a local job POSTs to it.
- **Port-forwarded (the important remote case).** `nodehammer serve` on lxplus
  (headless, no GPU); `ssh -L 8080:localhost:8080`; open the browser locally.
  The experiment job, also on lxplus, POSTs to `localhost:8080`. Because the web
  viewer is a fetch client, this needs no inbound connection to the laptop.
- **Bundled wasm.** The native binary embeds the wasm viewer assets (CMakeRC /
  generated byte arrays / appended zip) so `serve` needs no external web root.
  Build-graph wrinkle: the wasm viewer is a *separate* emscripten build, so this
  is two-stage — build wasm first (CI artifact), native build embeds it; gate on
  artifact presence with a `--web-root <dir>` fallback.

### 6.3 Where conversion runs

| Source | Preferred converter location | Mechanism |
|---|---|---|
| DD4hep/TGeo (have XML/recipe) | server (heavy flavor, in env) | POST path/recipe |
| GeoModel (have `.db`) | server (GeoModel flavor) | POST `.db` bytes/path |
| Live runtime-built geometry | **client, in the experiment process** | amalgamated header (§7) |
| Already have `.nhb` | neither | reference by hash |

---

## 7. Client-side conversion — the single amalgamated header

For live geometry (and for feeding a static server), the experiment converts
in-process and POSTs `.nhb`. This is packaged as a **single, layered, vendorable
header** — a build artifact assembled in CMake (sqlite/stb model), generated from
the same modular sources the CLI compiles normally. It is **idiomatic C++**, not
a C ABI: it is compiled from source in the experiment's own TU, so there is no
binary boundary to protect.

### 7.1 Layering by macro

- **Bare include (no macros)** → **connector only**: sockets + HTTP long-poll +
  event-payload building (hand-rolled JSON writer, no dependency). C++17.
- **`#define NH_CONVERT` + `NH_IMPORT_DD4HEP` / `NH_IMPORT_TGEO`** → additionally
  the semantic IR, the serializer, and the selected importer. C++20.

Load-bearing rule: the IR and all vendored heavy content sit **behind** the
convert gate, so bare-connector mode keeps its light footprint. Since the file
physically contains gated content even when compiled out, **produce two variants
from one source** — `nodehammer_connect.h` (slim) and `nodehammer_convert.h`
(full, includes the connector) — so connector-only users get the small file.

### 7.2 The stb one-TU pattern

`#define NH_IMPLEMENTATION` in **one dedicated `.cpp` that includes only this
header**. This solves ODR (definitions exist once) and *quarantines* the vendored
libs so they cannot clash with a different version the experiment uses elsewhere.
Declarations-only includes can go anywhere; definitions live in exactly one TU.

### 7.3 Vendor / shim / assume — the dependency plan

Evidence: the write slice (dd4hep + tgeo importers + `flatbuffer.cpp` serializer)
uses only `glm::dmat4/dvec4/dvec3/vec3` with **column-major element access, no
matrix math**; `unordered_dense` appears **only** as the four scene maps in
`semantic.hpp:248-251`, all keyed by `StrongId` which already has `std::hash`
(`semantic.hpp:43`); the serializer's internal dedup/remap maps are already
`std::map`/`std::unordered_map`. No ROOT dictionaries (`rootcling`/`LinkDef`)
anywhere. No `nlohmann` in the write slice. Only `zstd` is a compiled dep, and it
is only in the FB *read* path — not needed client-side.

| Dependency | Treatment | Why |
|---|---|---|
| **flatbuffers** (+ flatc-generated header) | **Vendor** (inline) | It *is* the `.nhb` output; absent from experiment envs |
| **glm** | **Shim** (~50 lines) | Slice only does column-major element I/O; real math lives in core/viewer, reached only via FB bytes |
| **ankerl/unordered_dense** | **Shim** (3-line alias to `std::unordered_map`) | Only `StrongId`-keyed scene maps; `std::hash` exists; byte-identical output not required |
| **DD4hep / ROOT / GeoModelIO** | **Assume present** (behind `NH_IMPORT_*`) | The experiment already has them; the whole point |

The glm shim swaps *which header is inlined* (a `glm_shim.hpp` providing
`namespace glm`), leaving `semantic.hpp:4` untouched — no `#define`-ing type
names. The modular CLI/viewer build keeps real glm. Net vendored payload:
**flatbuffers + two small shims.** Note the client's *event* JSON writer needs no
nlohmann (§3.2), so the slim connector has **zero** vendored/assumed libraries.

### 7.4 Live entry points

- **TGeo: already exists** — `import(TGeoManager*)`
  (`src/ir/tgeo/semantic/importer.hpp:38`); pass `gGeoManager`.
- **DD4hep: ~15-line refactor** — today file-path only
  (`fromCompact`, `src/ir/dd4hep/semantic/importer.cpp:105-106`); split the walk
  (`:116-142`) into a helper taking `TGeoManager&` + `DetElement` and add an
  `import(dd4hep::Detector&)` overload.

Producer usage:

```cpp
// nodehammer_impl.cpp — includes nothing else:
#define NH_IMPLEMENTATION
#define NH_IMPORT_DD4HEP
#include "nodehammer_convert.h"
```
```cpp
nh::SemanticScene scene = nh::importDD4hep(detector);  // or importTGeo(gGeoManager)
std::vector<uint8_t> nhb = nh::toNhb(scene);           // raw .nhb bytes
v.sendGeometry(nhb);                                   // POST /geometry
```

### 7.5 Standard floor

The slice is C++20-clean except one `std::print` in the DD4hep importer
(`importer.cpp:18`); everything else is C++20-or-lower (`std::format`, `std::span`,
`std::bit_cast`, `std::numbers`). **C++20 is the floor** for the convert path
(covers LCG_104+); the connector-only path holds at **C++17**. These specific
files must stay within their floor forever, enforced by CI (§10.3).

---

## 8. Config / filtering (experiment-authored)

Filtering is essential, not cosmetic — you cannot tessellate all of ATLAS, so
`SelectionEngine::prune()` must cut a GeoModel scene to a tractable subset before
tessellation. Crucially, this is **a text file the experiment authors itself**:

- Reuse the existing TOML `NHConfig` + `[[selection_rules]]` +
  predicate-expression grammar (`path ~= "**/Pixel/**" && tag.sensitive ==
  "true"`). No software to add.
- Transport it like geometry: `POST /config`. The viewer's `BuildSession`
  already resolves a root config key + geometry key and chases includes.
- **`config validate` already exists** — experiments iterate on the filter file
  in CI without launching the viewer.
- Presentation (per-collection colors, default visibility, hit size) is a natural
  sibling `[event_display]` / `[[collection_style]]` section in the *same*
  config, not a second file.

---

## 9. Build scenarios

### 9.1 CLI (modular) build — unchanged

The static and heavy CLI flavors compile the modular `.cpp/.hpp` normally:

```bash
./build.sh cmake --preset basic && ./build.sh cmake --build build   # static-ish, light
./build.sh cmake --preset dev   && ./build.sh cmake --build build   # heavy backends on
```

New CMake options mirror the existing gating: reintroduce
`NODEHAMMER_WITH_GEOMODEL`, add event-IR sources, and (for the server) an HTTP
option.

### 9.2 Amalgamated header generation (new CMake target)

A generated build artifact, from the same sources:

1. Run `flatc` → `semantic_generated.h` (already done for the normal build;
   `CMakeLists.txt`).
2. An amalgamator (e.g. [`quom`](https://github.com/Viatorus/quom) or an
   nlohmann-style script) inlines: our headers + the vendored flatbuffers runtime
   + the generated header + `glm_shim.hpp` + the unordered_dense alias; **leaves
   as `#include`**: standard library and env libs (`<DD4hep/...>`, ROOT); dedups
   via include guards; preserves the `#ifdef NH_IMPORT_*` gates.
3. Emit two variants: `nodehammer_connect.h` (slim) and `nodehammer_convert.h`
   (full).
4. De-duplicate the one shared anon-namespace helper (`tgeoMatrixToGlm`, defined
   in both `tgeo/.../importer.cpp` and `shape_dispatch.cpp`).

### 9.3 wasm viewer build

Separate emscripten toolchain build producing the GLES3 + WGPU viewer
executables. Native `serve` embeds these artifacts (§6.2).

### 9.4 Per-environment client compilation

The **amalgamated header** compiles in the *experiment's* env (their DD4hep /
ROOT / GeoModelIO, their compiler at ≥C++20 for convert, ≥C++17 for connector).
**No nodehammer build is needed there** — this is the whole point of shipping
source, not a binary.

---

## 10. Test scenarios

### 10.1 Unit (existing suite)

Catch2, auto-discovered (`./build.sh ctest --test-dir build`). New coverage:
event-IR round-trips, the JSON and FB event importers, the GeoModel importer,
`EventScene` overlay tessellation.

### 10.2 Golden equivalence for the amalgamation (critical)

Import a fixture geometry through **both** paths and assert the results match:

- Modular build (real glm, real unordered_dense) → `.nhb` A.
- Amalgamated header (glm shim, unordered_dense alias) → `.nhb` B.
- Assert **semantic equivalence** of the loaded scenes (not byte-identity, since
  the map alias changes iteration order). This guards the shims: a missing glm
  API surfaces as a compile error, a convention mismatch (column-major, identity
  ctor) as a test failure.

### 10.3 CI: compile the generated amalgamation

The single biggest anti-drift measure. A CI job **generates and compiles**
`nodehammer_convert.h` in C++20 against a real ROOT/DD4hep environment (the `dev`
spack env), and compiles `nodehammer_connect.h` bare at C++17. Without this, the
amalgamation silently rots while the modular build stays green.

### 10.4 Conformance kit (adoption multiplier)

Publish `event.fbs` + example JSON payloads + a `validate-event` command
(mirroring `config validate`). Because JSON is validated against the schema
(§3.2), an experiment answers "does my event conform?" in isolation, in CI,
without installing the viewer. Worth more for adoption than any transport feature.

### 10.5 Transport / server tests

- Rendezvous logic: a mock producer POSTs, a mock viewer advances, assert the
  blocking POST releases with the right action; viewer-disconnect → `Quit`.
- Content-type dispatch: same event as JSON and as binary FB → identical scene.
- `serve` mode: asset + `.nhb` fetch, `GET /wait-event` / `POST /advance` cycle.

### 10.6 End-to-end smoke

Extend `test.sh` (currently ODD `convert`): start `serve`, POST
the ODD geometry + a hand-authored example event JSON, assert the pipeline builds
a render scene. For the client path, a small program using the amalgamated header
against the ODD DD4hep compact, POSTing `.nhb`, driven by the blocking loop.

---

## 11. Usage patterns (the friction ladder)

Ordered by how little the experiment must adopt. All feed the same server /
blocking loop over the same wire protocol.

| Tier | Build-time surface | Runtime surface | For whom |
|---|---|---|---|
| **0 — protocol only** | *nothing* | HTTP client they already have (`curl`, `requests.post`) + documented JSON | first contact, other languages, "let me test it" |
| **0.5 — data they already emit** | *nothing* | ship `edm4hep` / a `.db` / a compact-XML path; server converts | Key4hep / ATLAS |
| **1 — vendored connector** | one file, `nodehammer_connect.h` (idiomatic C++17, zero-dep) | server on `PATH` / reachable | C++ producers wanting typed helpers, not a dependency |
| **2 — convert in-process** | `nodehammer_convert.h` (compiled in their env, C++20) | — | live/runtime-built geometry |

Heavier options (a C API, a prebuilt client `.so`, an edm4hep companion header)
are deliberately **not** on this ladder; see §12.

### 11.1 Concrete flows

**Physicist, laptop, offline scan.** `nodehammer view odd.nhb`; drag in event
`.nhe`/NDJSON files, step through. Zero experiment involvement.

**Other-language producer (Python analysis / notebook).** `requests.post` events
as JSON against `/event`, drive the loop from Python. No binding, no build change.

**edm4hep experiment (Key4hep/FCC).** They already write edm4hep — hand the file
to a DD4hep-linked `nodehammer` (geometry from the compact-XML path, events from
the edm4hep importer). No client code.

**ATLAS, live GeoModel + reco loop.** GeoModel `.db` (path on EOS, or serialized
from the live tree via their GeoModelIO) → `POST /geometry`; a hand-authored TOML
filter → `POST /config`; then the reco loop builds hits/tracks and drives the
blocking loop, viewing in a port-forwarded browser (`serve` on lxplus).
Build-time footprint: at most one vendored header.

**Runtime-built / aligned geometry.** No file to reference → amalgamated
`nodehammer_convert.h` converts the live `TGeoManager*`/`Detector&` in-process →
`.nhb` → `POST /geometry`; events as above.

---

## 12. Deferred / optional extensions (explicitly off the main path)

None of the following is required for a working event display. Each is a possible
later addition once the JSON + amalgamated-header path is proven; they are
recorded here so the main path stays lean.

- **A C API around the amalgamated header.** Straightforward to add later on top
  of the C++ core if in-process, non-C++, non-HTTP bindings are ever wanted. The
  wire protocol already covers other languages, so this is low priority.
- **Prebuilt `libnhclient.so` runtime accelerator.** A binary/zero-copy fast path
  `dlopen`ed from the connector, provisioned by the environment (never a build
  dep). It is the *only* component that would reintroduce a C ABI, and for
  human-paced display the JSON/HTTP path is fast enough — so it is deferred.
- **edm4hep companion header** mapping edm4hep collections → connector calls
  using the experiment's existing edm4hep dependency. Convenience over the
  server-side edm4hep importer; add if demand appears.
- **Arrow event importer + C Data Interface (server-side).** Zero-copy, zero-link
  ingestion for very large columnar events or hand-off to analysis tooling. Slot
  into the registry alongside JSON/FB when a real need appears.
- **Native GDML importer** (plain XML, no Geant4/ROOT) so a *static* server can
  ingest geometry with zero environment. Pragmatic escape hatch, but GDML export
  tends to drop assemblies/reflections and the source-specific tags filtering
  relies on — hence not primary.
- **`LD_PRELOAD` auto-capture** (interpose a framework's geometry-finalize call to
  grab the world with zero code change). A demo trick, too fragile for
  production; recorded only for completeness.

---

## 13. Open questions

- **Live vs. online priority.** Offline scanning (file, playback, navigation)
  vs. online monitoring (live stream) have different UX. Which is primary?
- **Geometry binding.** Content hash (reproducible, producer must know it) vs.
  logical name/version (looser). Lean hash + name fallback.
- **`Prev` support.** Only meaningful if producers are random-access; skip in v1?
- **Security.** `serve` binds localhost by default; token auth for remote.

---

## 14. Suggested phasing (de-risk cheapest first)

1. **Event IR + `schemas/event.fbs`** (explicit units/frame/geometry-ref) + JSON
   event importer (schema-validated) + viewer file-load + hit/track overlay.
   Validate against a hand-authored example over ODD.
2. **HTTP `POST /event` + blocking rendezvous** (reuses the JSON importer) +
   native `view`. Then `serve` + web `wait-event`/`advance`.
3. **`nodehammer_connect.h`** (slim, connector only, idiomatic C++17). Conformance
   kit + `validate-event`.
4. **Amalgamation generator + `glm_shim.hpp` + unordered_dense alias**, the
   golden-equivalence test, and the CI compile job. DD4hep `import(Detector&)`
   refactor.
5. **Heavy converters against the proven contract**: GeoModel importer (server),
   optional binary-FB event encoding, edm4hep importer.

The riskiest claims (shim correctness, amalgamation compiling in a real env, the
DD4hep live entry) are cheap to prove and should come early. Everything in §12
stays deferred until this path works end-to-end.
