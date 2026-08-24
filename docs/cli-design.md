# The nodehammer CLI — one vocabulary, five front doors

> Status: design of record. Nothing here has shipped; the implementation plan is
> §9. Written at 0.2.0rc3, deliberately before 1.0, because command names are the
> one part of a tool that cannot be refactored quietly afterwards.

## 1. Where it stands

Nine commands, grown one at a time, each reasonable on its own:

| command | options |
|---|---|
| `convert` | `-i` `--input-format` `-c` `-o`(1..n) `--output-format` `--strict` `--timing` `--angle-cut START END` `--angle-cut-margin` |
| `inspect summary\|tree\|tags` | `-i` `--input-format` `--color`; `tree`: `--depth/-d` `--filter/-f` |
| `validate-config` | `-c` |
| `config-flatten` | `-c` `-o` `--no-validate` |
| `config-lua` | `-c` `-o` `--no-validate` |
| `dump-semantic` | `-i` `--input-format` `-c` `-o` `--output-format {json,nhb}` `--rich` `--depth/-d` `--filter/-f` `--color` `--size-report` |
| `dump-render` | `-i` `--input-format` `-c` `-o` `--synthetic-box` |
| `viewer` | `path` `-c` `-i` `--title` + ~20 native window options + `--web` `--port` `--host` `--no-browser` `--web-assets` |

The option *spellings* are in good shape. `-i/--input`, `-c/--config`,
`-o/--output`, `--input-format`, `--output-format`, `--color`, `--depth/-d`,
`--filter/-f` mean the same thing everywhere they appear. That is the part to
lock down rather than change; everything below concerns the command names and
the vocabulary underneath them.

## 2. Nine findings

**F1 — Three naming schemes, at once.** Bare verb (`convert`, `inspect`),
verb-noun (`validate-config`, `dump-semantic`, `dump-render`), noun-verb
(`config-flatten`, `config-lua`). `validate-config` and `config-flatten` are both
about configs and are ordered opposite ways.

**F2 — `viewer` is a noun in a list of verbs.** The only one.

**F3 — `convert` and `dump-render` are one command with a different hardcoded
writer.** Both import, apply config, lower to render IR, write. `convert` picks
gltf/obj from `RenderExporterRegistry` (`cmd_convert.cpp:206`); `dump-render`
hardcodes JSON.

**F4 — `inspect tree` and `dump-semantic --rich` are one feature, declared
twice.** Same `--depth`, `--filter`, `--color` in both (`cmd_inspect.cpp:278`,
`cmd_dump_semantic.cpp:210`).

**F5 — The CLI cannot write `.nhr`; the Python API can.**
`RenderScene::formats()` returns `nhr, gltf, obj` (`api/render_scene.cpp:67`) and
`RenderScene::write` special-cases `.nhr`/`.nhr.zst` *before* consulting the
registry (`:29`). `convert` bypasses `RenderScene::write` and goes straight to the
registry, so `convert -o x.nhr` fails while `nh.RenderScene.write("x.nhr")`
succeeds. One object, two front doors, different answers about what exists.

**F6 — `nhb` has two names depending on direction.** Exporter `formatName() ==
"nhb"`; importer `formatName() == "flatbuffer"` (`ir/fb/semantic/importer.cpp:12`).

**F7 — `--angle-cut` means two incompatible things.** In `convert` it takes two
values and performs a Manifold boolean (`cmd_convert.cpp:111`). In `viewer` it is
a flag paired with `--cut-start`/`--cut-end`, and it is a shader effect
(`cmd_viewer_native.cpp:95`).

**F8 — `dump-semantic` is load-bearing but named as a debug tool.** It is the
only producer of the `.nhb` blob every project needs, so the mainstream
publishing path currently runs through a command named "dump".

**F9 — `refuseNativeOnlyOptions` exists only because two commands share one
name** (`cmd_viewer_web.cpp:156`): a hand-maintained runtime allowlist standing
in for a registration boundary.

## 3. The design

Four nouns and one flagship verb:

    nodehammer convert   …                     geometry in, anything out — the pipeline
    nodehammer inspect   summary|tree|tags     ask a question about an artifact
    nodehammer config    validate|flatten      TOML and Lua configs
    nodehammer project   pack|publish|info     .nhproj archives
    nodehammer viewer    open|serve|shot|bench the app

Nouns are the stable vocabulary. The README's table — working set, archive,
project manifest, sidecar, steer, package — has not churned, while the verbs kept
multiplying. Grouping on nouns is what stops the next five commands from needing
a tenth naming scheme.

### 3.1 `convert` absorbs both `dump-*`

The output format already decides how far down the pipeline a run goes: `.nhb`
needs import plus selection; `.gltf` needs tessellation as well. So "dump the
semantic IR" and "convert to nhb" are the same request, and there should not be
two spellings for it.

    nodehammer convert -i odd.xml -c scene.toml -o odd.nhb.zst   # was dump-semantic
    nodehammer convert -i odd.xml -c scene.toml -o odd.json      # was dump-render
    nodehammer convert -i odd.xml -c scene.toml -o odd.glb       # unchanged

`-o` is already `expected(1, -1)`, so `-o odd.nhb -o odd.glb` in one run reads
naturally: run to the deepest stage any output asks for, writing each on the way
past.

**Fidelity belongs in the help text, not in the verb.** Tessellation is lossy by
construction — once solids are triangles the solids are gone — so `-o x.nhb` is
the faithful stop and `-o x.glb` is a one-way door. That is what tells a user
which artifact to archive, and it should be said where they will read it.

`--rich`/`--depth`/`--filter`/`--color` do not come along; that is `inspect
tree`, which already exists. `--size-report` stays (it describes the nhb
encoding). `--synthetic-box` is a test affordance, to keep or drop on its own
merits.

This depends on routing `convert` through `RenderScene::write` rather than the
bare registry — the same change that fixes F5.

### 3.2 `inspect` is a projection, and it has a machine format

The distinction between `inspect` and `convert` is **whole versus part**, not
human versus machine:

- `convert` translates an entire artifact into another representation.
- `inspect` answers a question about one — counts, the tag index, a subtree.

A tool-using caller asking "what tags are in this detector" does not want the
whole semantic IR as JSON; it wants `inspect tags`. So the projections need a
structured format of their own:

    nodehammer inspect tags --output-format json
    nodehammer inspect tree --filter '/Tracker/**' --depth 3 --output-format json

**Default `text`, never TTY-auto.** The tree already flips *presentation* on
TTY-ness — `Pager` no-ops when stdout is not a TTY (`pager.cpp:87`), `--color
auto` resolves to never when piped — and both are safe because they are
presentational. Flipping *structure* on TTY-ness is not: a pipe added later
silently breaks a parser. The piping half already works today; only the format is
missing.

**The JSON shape is API.** Callers pattern-match on it, so it carries a `schema`
integer the way `nh_runtime.json` does, and changes to it are versioned.

### 3.3 `config flatten` absorbs `config-lua`

Their own help text already says the same thing: both emit flattened TOML,
"round-trippable through `ConfigLoader`". They differ only in the input language,
which the extension already states.

    nodehammer config flatten -c scene.toml    # inline the include chain
    nodehammer config flatten -c scene.lua     # evaluate, then the same

### 3.4 `project pack | publish | info`

The capability that is missing outright: nothing shipped writes a `.nhproj`. The
three producers that exist are the viewer's GUI, `viewer --web`'s temp staging
directory (`cmd_viewer_web.cpp:221`, deleted when the server stops), and
`scripts/make_nhproj.py` (repo-only, hardcoded to `fixtures/configs/`).

    nodehammer project pack    -c scene.toml -i odd.xml -o odd.nhproj
    nodehammer project publish odd.nhproj -o site/
    nodehammer project info    odd.nhproj

**`-i` means input geometry in any importable format**, exactly as in `convert`
and `inspect`. `.nhb`/`.nhb.zst` is already one of them
(`ir/fb/semantic/importer.cpp:12`), so this is not an overload — merely a fast
path: already a blob, embed the bytes; anything else, import it and write
`<stem>.nhb.zst` into the archive. Without this, publishing a detector requires
`dump-semantic` as an intermediate step, which is F8 in its most visible form.

**`--root DIR` is explicit.** Today the archive's mount point is
`commonAncestor(config.parent, geometry.parent)` (`web/stage.cpp:81`) — invisible,
and it decides the archive's key space. Keys are the archive's public surface,
since includes resolve against them, so moving the geometry one directory up
silently rewrites every key. Default to the common ancestor, allow an override,
print what was chosen.

**Keep both refusals the current code gets right:** only one of `-c`/`-i` given
(`stage.cpp:150`), and any include failing to resolve (`stage.cpp:88`). The second
is the one that matters — a partial scene fails in a browser, on someone else's
machine, with no mention of this one.

`publish` is `stageRoot` pointed at a real directory instead of a
`ScopedStagingDir`. It closes the open item at `viewer-project-strategy.md:575`
("the CLI writes the same folder *only if* the native distribution ships the web
runtime"), which is now true: the runtime ladder and the `nodehammer-web` wheel
have landed.

### 3.5 `viewer open | serve | shot | bench`

    nodehammer viewer open  [PROJECT] [window/camera options]
    nodehammer viewer serve [PROJECT] [--port --host --no-browser --web-assets]
    nodehammer viewer shot  [PROJECT] -o out.png [--width --height --supersample]
    nodehammer viewer bench [PROJECT] -o results.json [--bench-scale]

The rule underneath: **separate subcommand when the option sets are disjoint; a
backend flag when they are shared.** `open` and `serve` are disjoint — twenty
window options against `--host`/`--port`/`--no-browser`/`--web-assets` — and F9 is
the runtime cost of pretending otherwise. `shot`'s two possible backends share
their whole option set, so that one is a flag (§4.2).

`shot` and `bench` stop being flags that silently change what the command *is*.
Today `--screenshot` turns "open a window" into "render and quit"; as a
subcommand that is simply the command's name.

`bench` could plausibly be hoisted to the top level as a dev tool. It is kept
here because it takes the same camera and quality options, and duplicating
fifteen options to make a taxonomic point is a bad trade.

## 4. Boundary conditions

### 4.1 What the viewer command must satisfy

| # | Condition | Where it lives today |
|---|---|---|
| B1 | Two binaries, one command: the library owns the subcommand, the executable extends it | `cmd_viewer_native.cpp:69` ("Extends, does not create") |
| B2 | No native viewer → a helpful error, not "unknown command" | `cmd_viewer_web.cpp:294` |
| B3 | Emscripten registers nothing — under wasm the module *is* what is served | `run.cpp:112` |
| B4 | Native-only options must not silently no-op under web | `refuseNativeOnlyOptions`, `cmd_viewer_web.cpp:156` |
| B5 | Two headless modes that quit: screenshot, benchmark | `cmd_viewer_native.cpp:132`, `:146` |
| B6 | Reentrant from Python: SIGINT scoped and restored, staging removed on interrupt | `cmd_viewer_web.cpp:122` |
| B7 | Packaging is the GUI entry point (`.desktop`, `.app`, shortcut) — bare `nodehammer` prints help | `main.cpp`, issue #74 |
| B8 | Input shapes: a `.nhproj`, a directory, loose `-c`+`-i`, or nothing | `cmd_viewer_web.cpp:181` |
| B9 | Web needs the runtime ladder, and its failure needs the ladder explanation | `cmd_viewer_web.cpp:211` |
| B10 | Exit-code propagation: a nonzero app rc becomes `CommandFailure` | `cmd_viewer_native.cpp:44` |

How the split meets them:

- **B1** falls out exactly. The library registers `viewer` and `serve`; the
  executable fills in `open`, `shot`, `bench`.
- **B4 dissolves.** `serve` never registers window options, so `viewer serve
  --screenshot` fails at parse with CLI11's own message. The allowlist is deleted.
  This is the strongest single argument for the split.
- **B2** uses the pattern already in the tree: the library registers
  `open`/`shot`/`bench` as stubs whose callback throws the helpful error, and the
  native half looks them up and replaces the callback — what it does today for
  `viewer` itself.
- **B5** becomes command names rather than mode-switching flags.
- **B7** is either `Exec=nodehammer viewer open`, or bare `viewer` kept working
  with `require_subcommand(0, 1)` defaulting to `open`. Both acceptable; the
  second is friendlier to type and costs one line.
- **B3, B6, B8, B9, B10** are unaffected — they live inside command bodies, not
  in the naming.

### 4.2 Extension point: a web screenshot

Speculative, but the design must not foreclose it. A browser-driven screenshot
has the *same* option set as the native one (`--width`, `--height`,
`--supersample`, camera/steer) and the same deliverable, a PNG at `-o`. By the
rule in §3.5 that makes it a backend selector, not a separate command:

    nodehammer viewer shot detector.nhproj -o shot.png --width 3840 --web

The binary seam accommodates it with no rework. Native `shot` needs the GPU stack
and so lives in the executable; web `shot` needs the static server and a browser
but no GPU, and so lives in the library. That means `shot` is registered by the
library and extended by the executable — the B1 pattern again. **The stub the
library must register anyway for B2 is the same slot the web implementation later
fills**: a throw becomes a body, and nothing is restructured.

It would reuse three things that exist — `stageRoot`, `serve`, and the viewer's
own PNG export — leaving browser automation as the only new dependency, whose
"no headless browser available" failure is ladder-shaped like `locateRuntime`'s.
Worth noting what it buys: a wheel with no GPU stack could then produce high-res
PNGs headlessly.

Keep `--web` spelled as one word meaning "use the wasm runtime in a browser"
wherever it appears. `serve` needs no such flag because serving is possible only
that way; `shot` takes one because both backends are real.

## 5. One word per concept

| concept | canonical | what to fix |
|---|---|---|
| the file fed in | **input** (`-i`) | consistent already |
| semantic scene binary | **nhb** | importer calls it `flatbuffer` (F6); rename it |
| render scene binary | **nhr** | unreachable from the CLI (F5); route `convert` through `RenderScene::write` |
| the archive | **project** (`.nhproj`) | docs alternate with "archive". *project* is the CLI word; *archive* is the format word |
| the deployable folder | **package** | "publish package", "staged root", "static folder" all in use |
| view state | **steer** | consistent already; put it on the CLI (§6) |

## 6. One verb, one meaning

- **validate** — check and report; writes nothing
- **flatten** — resolve to a self-contained equivalent
- **convert** — read one form, write another
- **inspect** — answer a question about an artifact; never writes a file
- **pack** — many files into one archive
- **publish** — an archive into a deployable folder
- **open** / **serve** — run the app locally / over HTTP
- **shot** / **bench** — render once and quit / measure and quit
- **info** — one screen of facts about a single artifact

**`dump` disappears.** Everything it named is either `convert` (machine-readable
output) or `inspect` (a projection) — and blurring those is exactly how the only
producer of `.nhb` came to be called a dump (F8).

**Fix `--angle-cut` while renaming (F7).** They are different operations. The
`convert` help text already calls its own a *wedge* cut, so:

    nodehammer convert --wedge-cut START END      # Manifold boolean, geometry
    nodehammer viewer open --cut-start --cut-end  # shader effect, steer

Making camera and cut options a shared **steer** group across `open`, `shot`,
`bench` and `serve` puts the docs' existing word on the CLI, and lets `viewer
serve --camera-distance 500` bake initial steer into the sidecar. The sidecar is
already designed to carry it (`viewer-project-strategy.md:562`); `stage.cpp`
writes only archive/lock/title so far.

## 7. Two things `project` needs first

**`serialize()` is not deterministic.** A from-scratch archive puts every entry
in `impl_->overrides`, an `unordered_map` (`zip_working_set.cpp:52`), iterated
straight into the writer (`:261`), and `mz_zip_writer_add_mem` stamps the current
time. Two identical packs differ byte for byte, so the `project.<hash>.nhproj`
cache-bust that `make_nhproj.py` exists to provide cannot be reproduced in C++.
Sorting the keys and pinning the timestamp is what that script already does, and
is strictly better for the viewer's Save as well.

**Packing is not a web concern.** `packLooseFiles` sits in `nodehammer::web`'s
anonymous namespace (`web/stage.cpp:67`), but native "Save as archive" wants the
same function. Promote it to `src/project/pack.{hpp,cpp}` and give it its own
diagnostic code — today it throws `kFatalWebStage` (NH1003,
`diagnostic_codes.hpp:154`), which reads wrong under `project pack`.

Neither blocks the CLI work: `ZipWorkingSet` and `buildArchiveWorkingSet` are
already unconditionally in `NH_CORE_SOURCES` (`CMakeLists.txt:509`), deliberately
not gated on `NODEHAMMER_WITH_VIEWER`, so a new command can include
`viewer/archive_export.hpp` with **no CMake changes** — as `web/stage.cpp` already
does. `FilesystemProjectFs`, which packing mounts, is under `if(NOT EMSCRIPTEN)`
(`CMakeLists.txt:474`), so `project` registers beside `viewer` in `run.cpp`'s
`#ifndef __EMSCRIPTEN__` block.

## 8. Migration

**There is none, by decision.** The old spellings are removed outright rather
than kept as deprecated aliases.

`nodehammer` is published on PyPI (0.2.0rc3) and every release so far is a
pre-release, so nothing has promised a stable command line yet. An alias window
would buy a caller one minor release of grace at the cost of carrying two names
for every renamed command through the whole redesign — and the aliases would be
the last thing anyone removed. Pre-1.0 is exactly when a tool gets to change its
mind cheaply; the point of doing this now is not to have to be gentle about it.

| old | new |
|---|---|
| `convert` | unchanged |
| `inspect *` | unchanged |
| `validate-config` | `config validate` |
| `config-flatten` | `config flatten` |
| `config-lua` | `config flatten` |
| `dump-semantic` | `convert -o *.nhb` (or `inspect tree`, for `--rich`) |
| `dump-render` | `convert -o *.json` |
| `viewer` | `viewer open` |
| `viewer --web` | `viewer serve` |
| `--angle-cut START END` (convert) | `--wedge-cut START END` |
| `--input-format flatbuffer` | `--input-format nhb` |

Four test surfaces move together, and each exists for a reason the others cannot
serve: `tests/cli/test_cli_run.cpp` (in-process `cli::run`),
`tests/CMakeLists.txt`'s ctest entries over the built executable (the only place
the native `viewer` half is reachable), `tests/python/test_cli.py`, and
`tests/public/test_public_cli.cpp` (the installed shared library).

## 9. Implementation plan

Seven phases, each landing as its own commit. Ordering delivers the missing
capability first, then converges the vocabulary — the renames are the part that
can wait, and the part most improved by having `project` already in the tree to
name things against.

Each phase is developed in a worktree off `origin/main` and landed via PR.

### Phase 1 — `project pack | publish | info`

New surface, no renames, no migration. Unblocks headless `.nhproj` production.

1. Promote `packLooseFiles` from `web/stage.cpp`'s anonymous namespace to
   `src/project/pack.{hpp,cpp}`; `stage.cpp` calls it. Add `--root`. Add
   `kFatalProjectPack` to `diagnostic_codes.hpp`; keep `kFatalWebStage` for what
   stage.cpp still raises itself.
2. Make `ZipWorkingSet::serialize()` deterministic — sort the override keys, pin
   the entry timestamp. Assert byte-identical output across two serializations of
   the same content in `tests/viewer/test_zip_working_set.cpp`.
3. Teach `pack` to import non-`.nhb` `-i` through the semantic importer registry
   and write `<stem>.nhb.zst` into the archive.
4. `src/cli/cmd_project.cpp` with `pack`/`publish`/`info`; register in `run.cpp`
   inside `#ifndef __EMSCRIPTEN__`; declare in `run_internal.hpp`; add to
   `NH_CORE_SOURCES`.
5. `publish` calls `stageRoot` against a caller-named directory.
6. Tests: `tests/cli/` cases for pack round-trip, the both-or-neither refusal, the
   unresolvable-include refusal, and determinism.

**Done when** `nodehammer project pack -c fixtures/configs/odd.toml -i odd.nhb.zst
-o /tmp/odd.nhproj && nodehammer viewer --web /tmp/odd.nhproj` works, and
`project publish` reproduces what `scripts/stage_wasm_viewer.sh copy` produces.

### Phase 2 — the format layer

Independently valuable; makes the CLI and the Python API agree.

1. Route `convert` through `RenderScene::write` instead of
   `RenderExporterRegistry` directly, so `-o x.nhr` works and `--output-format`
   matches `RenderScene::formats()`.
2. Rename the semantic importer's `formatName()` from `flatbuffer` to `nhb`.
3. Tests: `convert -o x.nhr` round-trips; `nh.RenderScene.formats()` and
   `convert --help` list the same set.

### Phase 3 — `inspect --output-format json`

Additive; no renames.

1. `--output-format text|json` on the `inspect` group, default `text` —
   the spelling `convert` and `dump-semantic` already use for the format of
   what they emit, and unambiguous beside `inspect`'s existing `--input-format`.
2. A `schema` integer in every JSON document.
3. Bypass the pager for `--format json`.
4. Tests: each of `summary`/`tree`/`tags` parses as JSON and carries `schema`.
5. Document the shape in `docs/` — it is API from the moment it ships.

### Phase 4 — the `config` group

1. Merge `cmd_config_flatten.cpp` and `cmd_config_lua.cpp` into
   `src/cli/cmd_config.cpp` with `validate` and `flatten`; `flatten` dispatches on
   the input extension.
2. Delete `cmd_validate_config.cpp` and the two old registrations.
3. Tests: the new spellings work; the old ones are gone, which `cli::run` reports
   as an unknown subcommand rather than silently.

### Phase 5 — `convert` absorbs the dumps

The largest semantic change; depends on Phases 2 and 4.

1. `convert` accepts `.nhb`/`.nhb.zst`/`.json` outputs and stops at the shallowest
   stage that satisfies every requested output.
2. Multi-output across stages: `-o a.nhb -o b.glb` runs once to the deepest.
3. Move `--size-report` onto `convert`; decide `--synthetic-box` on its merits.
4. Delete `cmd_dump_semantic.cpp` and `cmd_dump_render.cpp`, tree printer
   included — `inspect tree` already covers `--rich`.
5. Add the fidelity note (§3.1) to `convert --help`.
6. Tests: every old `dump-*` invocation in `tests/python/test_cli.py` is
   rewritten to its new spelling.

### Phase 6 — the `viewer` split

The most code motion, and the least coupled to the rest.

1. Library registers `viewer` with `require_subcommand(0, 1)`, `serve`, and
   throwing stubs for `open`/`shot`/`bench`.
2. `cmd_viewer_native.cpp` looks up each stub, adds its options, replaces the
   callback.
3. Delete `refuseNativeOnlyOptions`.
4. `--screenshot`/`--bench` flags become the `shot`/`bench` subcommands; bare
   `viewer` falls back to `open`.
5. Rename `convert --angle-cut` to `--wedge-cut` (F7).
7. Tests: the executable-level ctest entries in `tests/CMakeLists.txt` gain a case
   per subcommand; assert `viewer serve --screenshot` fails at parse.

### Phase 7 — the vocabulary, everywhere else

1. README's CLI block, `docs/*` that name old commands (`event-display-design.md`,
   `viewer-single-instance.md`, `error-model.md`, `config-scripting-lua.md`).
2. `Justfile` recipes, `scripts/*`, CI workflow invocations.
3. Retire `scripts/make_nhproj.py` in favour of `project pack`, or reduce it to a
   thin wrapper.

### Sequencing notes

- Phases 1, 2, 3 are independent of each other and of the renames. Any can land
  alone.
- Phases 4, 5 and 6 are independent of each other; any order works. With no
  alias layer to establish first, nothing gates anything.
- Phase 7 is last by definition, but each earlier phase updates the tests and help
  text it touches rather than deferring all of it.

### The one risk worth naming

`nodehammer` is published on PyPI (0.2.0rc3), so the old spellings may already be
in someone's script, and §8 gives them no grace period. That is the accepted
cost: every release so far is a pre-release, and the same change after a stable
one costs a major version instead of a note in the changelog.
