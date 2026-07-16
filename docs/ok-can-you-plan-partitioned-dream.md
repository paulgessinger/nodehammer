# Plan: Reshape the web project model around "the archive is the project" — authoring + publishing (supersedes old §12 steps 8–9)

## Context

The original ask was "plan step 8" — `WebBagProjectFs` (ZIP-in-IDB). Digging in
revealed the surrounding model (`docs/viewer-project-strategy.md` §3.2/§3.4/§6/§13)
was wobbly: web persistence was keyed on an implicit "is there a manifest?" binary
driving a single global IDB slot, graduation was a one-way lossy URL→bag swap, and
"manifest edits evaporate on reload" was the accepted behavior. **Step 7 already
made `ArchiveProjectFs` cross-platform** (unbound archives work on web, save via
download), which changes the ground truth: every web project is really a
`ZipWorkingSet` you can serialize and publish.

The reshaped thesis (agreed with the user): **the archive is the universal project
unit — for authoring (edit/curate/drop) and publishing (serialize → host → share).**
The URL "manifest" collapses into a thin **sidecar** that points at an archive. This
document is the new design of record; the strategy doc gets rewritten to match
(that rewrite is step R0 below).

## Decisions locked (from design discussion)

- **Open/drop a `.zip` on web → a live unbound `ArchiveProjectFs`** (reuse the
  cross-platform backend; preserves folder structure; Save = download). *Not* the
  old "flatten into a web bag."
- **Retire the web `BagProjectFs` entirely.** It provides only one thing the
  working set doesn't — **basename-fallback resolve** for flat loose-file drops
  (`materials/common.toml` → `common.toml`). Carry that forward as a **resolve
  policy** on the working-set backend, switched on by provenance (loose/dropped =
  on; opened/remote archive with real paths = off). Nothing else is lost.
- **Two web runtime postures, same wasm binary, branch on sidecar presence:**
  - **Application mode** (no sidecar): empty start, editable, native-like; the
    working set **auto-persists to IDB** and restores on reload.
  - **Viewer mode** (sidecar present): fetch the archive, present it, **content
    locked**, **re-fetched from source on reload** (IDB is *not* restored over it).
    Steer stays live + URL-committed.
  - This split *is* the resolution of "manifest edits should persist": app mode is
    your document (persists); viewer mode is a publication (reloads from source).
- **Persistence identity: single current document now, multi-ready.** IDB key
  `project/<id>` with a default id; "Save as archive" is the branch mechanism; a
  named multi-document switcher is a purely additive later step.
- **Content-lock lives in the sidecar only** (deployment property). The project
  manifest carries no lock; a `.zip` opened locally/native is always editable.
- **Preconfig is declarative TOML in the archive** (a *project manifest*). Lua-in-
  archive is a future escalation — Lua is deliberately out of the wasm closure
  (`CMakeLists.txt` gates `nodehammer_lua` behind `if(NOT EMSCRIPTEN)`) and running
  archive Lua on open is a trust/code-exec surface.
- **Steer (view-state) is a layered cascade**, most-specific-wins per key:
  `app default < archive [view] < sidecar overrides < URL query`. The live steer is
  written back to the URL (debounced, continuous) so the address bar always == the
  current screen (extends today's `commitUrlState`). A **sync panel** toggles what
  to sync, continuous vs manual, and whether to apply URL steer on load; its
  settings persist to `localStorage`.
- **Lock = content-immutable; view/steer is never frozen** (a shared posed link
  must keep camera/cut live). This is the "separate view-state from content" goal
  enforced.
- **Publishing: "Publish package"** emits a self-contained static folder droppable
  on any static host, zero server code (see "Publish, fleshed out" below).
  Content-hashed archive filename = free cache-bust. **Self-contained (bundle the
  runtime) now**; a "thin" variant (viewer.html loads js/wasm from a hosted,
  versioned, CORS runtime base) is additive later once such a host exists.
- **No fork-to-edit in viewer mode.** Viewer mode is strictly a presentation:
  locked, no drops/edits, never touches IDB. It offers **Save as archive**
  (download the `.zip`). Editing is a *deliberate separate step*: the user opens that
  downloaded `.zip` in the **application-mode** viewer. No implicit "duplicate to
  edit" — an in-place fork would be confusing (silent IDB write, unclear identity).
  Download → open-in-app is the only viewer→app bridge, and it's explicit.

## Terminology (lock before rewriting the doc)

| Term | Meaning |
|---|---|
| **Working set** | the live editable `ZipWorkingSet` that *is* the project (web always; native archive mode). |
| **Archive** | a serialized `.zip` of a working set — the portable, publishable content unit. |
| **Project manifest** | root `nodehammer.toml` *inside* the archive: `[project]` (entry config/geometry) + `[view]` (initial steer). Self-describes the archive. |
| **Sidecar** | the existing `nh_manifest.json` next to `viewer.html` (already fetched by convention on load, 404 → app mode), repurposed: registry of archive(s) + default + deployment presentation (lock, steer overrides). Replaces its old per-key `input`/`config` schema. |
| **Steer** | camera / angle-cut / rotation / toggles / selected-archive — the ephemeral per-link layer in the URL query. |
| **Provenance** | `Empty \| Local(name) \| Remote(url)` — where the working set came from; drives persistence + posture. |
| **Package** | the self-contained deployable folder emitted by "Publish". |

Modes collapse to two *substrates* — **Filesystem** (native live folder) and
**Working set** (everywhere) — plus the web *posture* (Application / Viewer).
Retired: `BagProjectFs` (web), per-key `UrlProjectFs`, "bag" and "URL session" as
distinct modes.

## Scope of unification

- **Web unifies fully now**: the web project is always a working set (provenance
  `Empty/Local/Remote`), IDB-persisted in app mode. Retire web `BagProjectFs` and
  per-key `UrlProjectFs`.
- **Native keeps its current substrates** (Filesystem, `NativeBagProjectFs`,
  `ArchiveProjectFs`) — the native app is the *reference* app mode is mimicking.
  Native additionally gains project-manifest reading and "Publish package". Retiring
  `NativeBagProjectFs` in favor of a native auto-persisted working set is a possible
  later symmetry, explicitly out of scope here.

## Concrete substrate

Build the working-set backend by **generalizing the existing `ArchiveProjectFs`**
(`include/nodehammer/viewer/archive_project_fs.{hpp,cpp}`), which already wraps
`ZipWorkingSet`, is cross-platform, and supports unbound mode + `serialize()` +
drops. Add:
- **Provenance** (`Empty | Local(name) | Remote(url)`) as a member + accessor.
- **Basename-fallback resolve policy** (port from `bag_project_fs.cpp:165-184`),
  enabled when provenance is `Empty`/`Local`-from-loose-drops.
- (web persistence + posture live in the App / platform layers, not the backend.)

`ZipWorkingSet` already has everything else: `create()`, `openFromBytes()`,
`read()` (lazy decompress), `writeEntry`/`removeEntry`, `listAtPrefix`,
`serialize()`, `dirty()`/`clearDirty()`.

## Staged implementation roadmap (each independently shippable)

**R0 — Strategy-doc rewrite + terminology.** Rewrite `docs/viewer-project-strategy.md`
§3–§8 and §13 around the archive-as-project model and the locked vocabulary;
re-scope §12 steps 8–9 into R1–R6. (No code.) *Do this first so the code steps have
a stable spec.*

**R1 — Unify the web project onto the working set (retire the web bag).**
- Generalize `ArchiveProjectFs` (provenance + basename-fallback resolve policy).
- `platform::makeEmptyBag()` on web (`src/viewer/platform_web.cpp:288`) → empty
  unbound working set instead of `BagProjectFs`.
- Route web ingestion (`webDropFetchCallback` `platform_web.cpp:303-323`;
  `nh_viewer_add_upload` `:502`): a single dropped/picked **`.zip`** →
  `setProject(ArchiveProjectFs(openFromBytes(bytes)))` (provenance `Local`); loose
  files → `writeEntry` into the current working set.
- Enable web **Open archive…**: unhide the menu item (`menu_bar.cpp`, currently
  `!kIsWeb`); implement `Platform::openArchivePicker()` (`platform_web.cpp:431`) as a
  `<input type=file accept=".zip">` that pushes bytes to a new
  `App::openArchiveFromBytes`. Add `.zip` to the picker `accept` list (`:251`).
- Delivers the deferred-from-step-7 "open an existing `.zip` on web".
- CMake: drop `bag_project_fs.cpp` from the web closure (keep for native as the
  transitional fallback until R-native, or retire if unused).

**R2 — IDB persistence + Application mode.**
- New IDB EM_JS bridge in `platform_web.cpp` (none exists today — only
  `localStorage` via `nh_viewer_*_persistent_text` `:37-61`): async
  `nh_viewer_idb_get(store,key)` → mallocs + calls an exported
  `_nh_viewer_idb_get_done(ptr,len)`; `nh_viewer_idb_put(store,key,ptr,len)`
  fire-and-forget; `nh_viewer_idb_delete`.
- Extend `Platform` (`platform.hpp`) with `loadPersistentBlob(key, cb)` /
  `savePersistentBlob(key, bytes)` / `deletePersistentBlob(key)` — web-backed;
  native no-op (or file-backed under `getDataHome()` if we want native symmetry).
- App mode: debounced `serialize()` → `savePersistentBlob("project/<id>", blob)`;
  cold-load `loadPersistentBlob` → `openFromBytes` → bump generation. Flush on
  `beforeunload` (reuse the existing `nh_viewer_save_persistent_state` hook shape,
  `platform_web.cpp:154-156`). Restore is suppressed when a sidecar is present
  (viewer mode) — a late async restore that finds `project()` no longer the app-mode
  working set drops its bytes.

**R3 — Sidecar → archive + project manifest (Viewer mode).**
- The branch already exists: `web/viewer.html` fetches `nh_manifest.json` on load
  (`:65`, `{cache:'no-store'}`, 404 → empty/app mode). Repurpose its schema from
  `{input, config, defaults}` to `{ archives: [...], default, lock, view? }`; the
  query string selects among `archives` + carries steer. When it names an archive,
  `viewer.html` hands the archive URL to `nh_viewer_start`; C++
  (`src/web/viewer_main.cpp:193-205`, today building `UrlProjectFs`) fetches the
  archive bytes → `ArchiveProjectFs(openFromBytes(...))` provenance `Remote`, viewer
  posture (locked per sidecar), no IDB restore, re-fetch on reload.
- **Project manifest** parser: read root `nodehammer.toml` from the working set;
  `[project]` → `setRootKeys(config, geometry)`; `[view]` → seed steer. Absent →
  today's recognition heuristics. Parse with tomlplusplus directly (small; not the
  full `ConfigLoader`).
- Legacy `input`+`config` manifests: support via a compat shim that fetches those
  two files into a working set, then **retire per-key `UrlProjectFs`**. (Perf note:
  archive is one eager blob download + lazy per-entry decompress; fine for the
  config+geometry common case.)

**R4 — Layered steer + URL + sync panel.**
- Resolve the steer cascade on load (`app < archive [view] < sidecar < URL`).
- Extend `app_state` + `commitUrlState` (`platform_web.cpp:21`, `app_state.cpp`) from
  camera+toggles to angle-cut / rotation / selected-archive; continuous debounced
  writeback so the URL always == current screen.
- **Sync panel** (new ImGui window): toggles for which steer keys sync, continuous
  vs manual ("Copy view link"), and apply-URL-steer-on-load; persist the panel's own
  settings via `savePersistentText`.

**R5 — Publishing ("Publish package").** See "Publish, fleshed out" below.
- **Web (app mode) — concrete, self-sufficing:** the running app is served from
  `basePath`; it fetches its own sibling runtime files (same-origin), generates
  `nh_manifest.json` + `project.<hash>.zip` (`serialize()`), and zips the set into a
  download the user drags onto any static host.
- **Native — gated on packaging:** the CLI writes the same package *only if* the
  native distribution ships the web runtime (staged e.g. under
  `share/nodehammer/web/`). Establishing that (build wasm → stage → native package
  includes it) is a packaging/CI task; until then native emits archive + sidecar and
  reports the runtime isn't bundled. (No fork-to-edit — removed by decision.)

**R6 — (Deferred) Named multi-document + thin runtime.** Workspace switcher (New /
Recent / Rename / Delete over `project/<id>` slots); thin package runtime once a
hosted versioned CORS runtime exists. Out of scope for this reshape.

## Publish, fleshed out

**Package contents** (a folder, or a zip of it for one-drag deploy):

```
viewer.html                              # the shell; fetches nh_manifest.json → viewer mode
nodehammer-gles3.js  / nodehammer-gles3.wasm
nodehammer-wgpu.js   / nodehammer-wgpu.wasm
nodehammer-compute.js/ nodehammer-compute.wasm
nh_manifest.json                         # sidecar: { archive: "project.<hash>.zip", lock, view }
project.<hash>.zip                       # serialized working set
```

The web build already produces the three runtime pairs (CMake install targets
`nodehammer-gles3/-wgpu/-compute`) and `viewer.html` already picks gles3-vs-wgpu at
runtime via `navigator.gpu`. Dropping this folder on any static host and visiting
`viewer.html` → fetch `nh_manifest.json` → **viewer mode** loads the archive. Zero
server code.

**Where the runtime bytes come from:**
- **Web (app mode):** the app is served from `basePath`, so it can `fetch()` its own
  siblings — `viewer.html`, all three `*.js` + `*.wasm` (names are known/enumerable)
  — same-origin, then add the generated sidecar + archive and zip → download. Always
  version-matched to the running app. **This is the concrete, self-sufficing path.**
- **Native:** the executable doesn't contain the web runtime, so native publish
  needs the native distribution to *ship* it (install the emscripten targets'
  output + `viewer.html` alongside the binary, e.g. under `share/nodehammer/web/`).
  The CLI then copies that staged runtime into the package. **Feasibility to
  establish** — it couples native packaging to a prior wasm build (build wasm →
  stage → native package includes it). The user flagged this as the CLI's job to
  determine; until it's wired, native publish emits only the archive + sidecar.

**Thin (later):** viewer.html gains an overridable runtime base URL so a package can
reference a hosted `nodehammer-*.{js,wasm}` instead of bundling them — needs a
durable, versioned, CORS-enabled runtime host; additive, out of scope now.

## Files touched (representative)

- `include/nodehammer/viewer/archive_project_fs.hpp` + `src/viewer/archive_project_fs.cpp` — provenance + basename-fallback resolve policy.
- `src/viewer/bag_project_fs.{hpp,cpp}` — retire from web closure.
- `src/viewer/platform_web.cpp` — IDB EM_JS bridge, `.zip` open input, `makeEmptyBag`, ingestion routing, steer writeback.
- `include/nodehammer/viewer/platform.hpp` + native platform TUs — `*PersistentBlob`, web `openArchivePicker`.
- `src/web/viewer_main.cpp` — sidecar startup branch.
- `src/viewer/app.cpp` / `app.hpp` — `openArchiveFromBytes`, provenance/posture, IDB persist loop, fork-to-edit, publish.
- `src/viewer/app_state.{hpp,cpp}` — extended steer serialization.
- `src/viewer/ui/menu_bar.cpp` + `ui/ui_context.hpp` — web Open archive, Publish, Duplicate-to-edit, sync panel.
- new: project-manifest parser; sidecar parser; sync-panel window; publish-package assembler.
- `web/viewer.html` — sidecar schema (`nh_manifest.json`) archive pointer; (later) overridable runtime base.
- `CMakeLists.txt` — web source gating; native packaging staging of the web runtime for native publish (R5, feasibility TBD).
- `docs/viewer-project-strategy.md` — R0 rewrite.

## Verification

- **Native:** `./build.sh cmake --preset dev && ./build.sh cmake --build --preset dev`; `ctest`.
- **Web:** `just wasm` build links the working-set backend + IDB bridge; no `BagProjectFs` in the web closure.
- **Unit tests:** working-set basename-fallback resolve policy; project-manifest parse (`[project]`/`[view]`, absent-fallback); sidecar parse (archive selection, legacy compat); steer cascade precedence.
- **End-to-end (web, `/run`):**
  - App mode: drop files → build → reload → **work restored from IDB**; open a `.zip`
    → structure preserved; Save as archive → download.
  - Viewer mode: serve a sidecar → archive loads locked, project manifest seeds the
    scene; orbit/cut → URL updates; copy URL in a second tab → identical screen;
    "Duplicate to edit" → editable local doc; Publish package → drop the folder on a
    static host → live viewer.

## Out of scope

- Named multi-document manager UI; thin package runtime + hosting infra (R6).
- Retiring `NativeBagProjectFs` / native working-set auto-persist.
- Lua-in-archive preconfig (wasm-Lua + trust surface).
- Editor windows + commit (old §12 steps 10–11) — unchanged, still after this.
