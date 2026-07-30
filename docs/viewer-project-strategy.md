# Viewer project + edit + save strategy

This document describes the target interaction model for the viewer's project
system, and connects that model to the existing
[`ProjectFs`](../src/viewer/project_fs.hpp) abstraction —
including which capabilities exist today, which need additions to (or
simplifications of) the base interface, and which are best expressed as new
concrete backends or decorators.

---

## 0. The model: the archive is the project

Steps 1–7 (§12) established the pieces; step 7 made `ArchiveProjectFs`
cross-platform. That changed the ground truth and the model was **reshaped**
around a single thesis:

> **The archive is the universal project unit.** One `ZipWorkingSet` is the thing
> you *author* (edit / curate / drop files into) and the thing you *publish*
> (serialize → host → share). Authoring and publishing are the same object flowing
> two directions.

This collapses the old separate **URL / bag / archive** web modes into one
working-set substrate. **Filesystem** stays a distinct mode (a native live folder,
its own source of truth). The old per-key URL "manifest" collapses into a thin
**sidecar** that points at an archive.

### Vocabulary (used throughout the rest of this doc)

This table is the canonical definition; the README carries a condensed copy for
readers who never open this document — keep the two in sync when a term changes.

| Term | Meaning |
|---|---|
| **Working set** | the live editable `ZipWorkingSet` that *is* the project (web always; native archive mode). |
| **Archive** | a serialized **`.nhproj`** (ZIP container; PK magic intact) of a working set — the portable, publishable content unit. No `.zip` projects, no legacy formats. |
| **Project manifest** | root `nodehammer.toml` *inside* the archive: `[project]` (entry config/geometry keys) + `[view]` (initial steer). Makes an archive self-describing on every platform. |
| **Sidecar** | the `nh_manifest.json` next to `viewer.html` (already fetched by convention on load; 404 → app mode). Points at archive(s) + carries deployment presentation (lock, steer overrides). |
| **Steer** (view-state) | camera / angle-cut / rotation / toggles / selected-archive — the ephemeral per-link layer that lives in the URL query. |
| **Provenance** | `Empty \| Local(name) \| Remote(url)` — where the working set came from; drives persistence + posture. |
| **Package** | the self-contained deployable folder emitted by "Publish". |

### Two web postures (same wasm binary, branch on sidecar presence)

- **Application mode** (no sidecar): empty start, editable, native-like; the working
  set **auto-persists to IndexedDB** and restores on reload.
- **Viewer mode** (sidecar present): fetch the archive, present it, **content
  locked**, **re-fetched from source on reload** (IDB is *not* restored over it).
  Steer stays live and is committed to the URL so a link reproduces the exact screen.

App mode is *your document* (persists); viewer mode is *a publication* (reloads from
source). Content-lock is a sidecar (deployment) property; **view/steer is never
frozen**. The only viewer→app bridge is explicit: Save-as-archive downloads the
`.nhproj`, which you open in app mode — there is **no in-place fork-to-edit**.

The detailed, staged implementation plan (R0–R6) lives in the approved plan file;
§3–§13 below are the design of record it builds against.

---

## 1. Where we are today

The viewer already has the right shape for this work; what's missing is mostly
a write path, a unified hierarchical listing model, an editor surface, and a
few mode-transition rules.

**Status:** Steps 1–7 of §12 have landed. The old steps 8–9 (`WebBagProjectFs`,
URL reload semantics) are **superseded** by the §0 reshape and re-scoped into the
R1–R6 roadmap (§12).
- Step 1: `DirNode::children` is gone; backends maintain per-directory
  caches keyed by `generation()`; the App's tree panel recurses via
  `project_->list(node.key)`.
- Step 2: `OpenedFile::bytes` is now a `ByteBuffer` (refcounted handle
  over a `shared_ptr<const vector<byte>>`); `FilesystemProjectFs` reads
  fresh from disk on every `resolve()` (no in-memory byte cache); bag
  and URL backends share their cached vectors by refcount-bump.
  `BuildSession` holds `ByteBuffer`s, dropping the immediate-copy
  protection it used to need.
- Step 3: `NativeBagProjectFs` wraps an inner `FilesystemProjectFs`
  pointed at a process-owned `temp_directory_path()` subdir; writes flow
  through `add`, bump `generation()`, and warn on basename collision.
  Cross-launch persistence (`state.json` session slot + `getDataHome()`)
  and atomic temp+rename writes are still deferred — see §12 step 3.
- Step 4: `WatchedFilesystemProjectFs` decorates `FilesystemProjectFs`
  and watches the mount root via wtr.watcher (Conan); a debounced
  on-disk change calls `inner->rescan()`, bumping `generation()` so the
  existing build path re-walks. Wired at all native folder entry points.
  See §12 step 4 for the threading/safety details.

### Existing `ProjectFs` surface (relevant parts)

```
poll() / status() / errorMessage() / name() / warnings()
planAddPath / planAddBytes / addPath / addBytes        // user-gesture ingestion
resolve(key) -> {Ready | Pending | Missing | Error}    // byte access
generation()                                           // cache-invalidation gate
list(dir) -> span<DirNode>                             // hierarchy snapshot
rescan()                                               // force re-walk
```

### Concrete backends

| Backend                  | Source              | `addPath/Bytes` | `list`         | `rescan`        | Notes                                                                  |
| ------------------------ | ------------------- | --------------- | -------------- | --------------- | ---------------------------------------------------------------------- |
| `BagProjectFs`           | drops + picks       | accept          | flat snapshot  | no-op           | last-write-wins on basename collision; in-memory only; resolve hands out a `ByteBuffer` (refcount bump) |
| `FilesystemProjectFs`    | on-disk dir         | reject (no-op)  | lazy per-dir   | drops per-dir caches + bumps `generation()` | reads from disk on every `resolve` and wraps a fresh `ByteBuffer`; OS page cache handles repeats |
| `UrlProjectFs` (web)     | `emscripten_fetch`  | reject          | lazy per-prefix | no-op          | lazy fetch on first `resolve`; cached bytes are stored as a `ByteBuffer`, resolve is a refcount bump |

### Decoration discipline (already documented)

The base header explicitly anticipates wrappers:

> Wrapping is an atomic swap at the App layer:
> `app.setProject(std::make_unique<Wrapper>(std::move(impl_->project_)))`.
> Inner heap objects survive the swap intact …

This is the lever for everything below. **WatchedFilesystemProjectFs** and
graduation paths fit cleanly as decorators or as new concrete backends
consumed via the same interface — no API churn at the App boundary.

### App-side state already wired

- [`App::setProject`](../src/viewer/app.cpp) clears root keys + session state
  and accepts any new `unique_ptr<ProjectFs>`. This is where every mode
  transition lands.
- [`BuildSession`](../src/viewer/build_session.hpp) reads
  `generation()` and re-walks on bump or `force_walk`. Any mutation that
  bumps `generation()` (write-through to disk, watcher reload, archive
  remount) already flows through the existing rebuild path.
- The newly hoisted polling loop in `App::Impl::onFrame` runs the
  pipeline regardless of whether a scene is loaded, so file additions and
  edits drive a rebuild even after the first scene renders.
- A platform persistent-text primitive is in place:
  [`Platform::loadPersistentText` / `savePersistentText`](../src/viewer/platform.hpp)
  store small TOML/ini blobs under the config dir on native
  (`sago::getConfigHome()/nodehammer/<key>`) and in browser
  `localStorage` on web. [`app_state.{hpp,cpp}`](../src/viewer/app_state.cpp)
  uses it to round-trip `ViewerConfigState` (panel visibility,
  cull/cut/PBR toggles, **camera**) to TOML — loaded on startup, flushed
  on quit; on web the same state is also mirrored into the URL query
  string via `Platform::commitUrlState`. This is *view* state, separate
  from project/bag **content**, but it's the same primitive the deferred
  bag `state.json` slot (§3.2, §12 step 3) will build on.

---

## 2. Core principles

1. **Bytes always flow through the project FS.** Drops, edits, and saves
   all go through `ProjectFs`. The build session and the editor each
   manage the lifetime of their own consumed bytes (build session copies
   into the build job; editor copies into its text buffer). The project
   itself does not need to keep bytes "live" for outside consumers across
   frames.
2. **Storage is platform-native.** The project persists — the native bag into the
   app data folder, the web working set (application mode) into IndexedDB.
   `resolve` reads from that storage. Persistence is a property of the storage
   backend, not a separate concern bolted on top.
3. **The storage layer is the cache.** Backends backed by a real storage
   tier (filesystem, native bag's storage dir) do **not** keep an
   in-memory byte cache on top — the OS page cache already does that
   job. Backends without a cheap re-read (URL fetch results; ZIP entries
   that paid a decompression cost) keep what they need resident, but
   only for that reason.
4. **`ResolveResult` carries a `ByteBuffer`, not a span.** `ByteBuffer`
   is a thin wrapper around a `std::shared_ptr<const std::vector<std::byte>>`
   exposing only a read-only `span()`. Storage-backed backends allocate
   a fresh vector per `resolve()` call; cache-backed backends (URL,
   `ZipWorkingSet`) share their cached vector by refcount-bump — same
   `ByteBuffer` shape, no API split. Consumers (BuildSession, editor)
   hold a `ByteBuffer` for as long as they need; the buffer survives
   any backend mutation because the `shared_ptr` keeps it alive even
   after the cache slot drops its own reference. The editor
   copy-on-writes into its own mutable buffer when the user starts
   editing, dropping its `ByteBuffer` handle once it owns a private
   copy. A future memory-mapped backend can plug a custom deleter into
   the same `shared_ptr` shape (munmap on last release) without touching
   any caller.
5. **Hierarchy is the universal listing model.** All backends expose a
   tree. Flat backends are simply trees that are one level deep. Lazy
   expansion is part of the contract so large filesystems and archives
   don't pay full-walk cost up front.
6. **Each backend owns its own save semantics.** No unified overlay layer.
   Filesystem saves individual files; bag has no save target other than
   archive export; archive (native) saves in place; URL is read-only.
7. **Mode transitions are explicit `setProject` swaps.** Promotions are
   user-initiated and irreversible within a session, with the previous
   project's relevant state migrated into the new one before installing.

---

## 3. Per-mode contract

### 3.1 Filesystem mode (native)

- **Source of truth**: on-disk files (`FilesystemProjectFs`).
- **Editing**: each opened file gets its own ImGui window with a save icon
  + ⌘S hotkey scoped to that window's focus. The editor's text buffer is
  the only "overlay" — saving flushes to disk via the project's
  `writeBytes(key, …)` call (see §5) and clears the dirty bit on that
  buffer.
- **External edits** (future): a watcher (FSEvents / `inotify` /
  `ReadDirectoryChangesW`) is added as a `WatchedFilesystemProjectFs`
  wrapping the existing backend. On modify it invalidates the affected
  entries (per-dir lazy cache and any byte cache for the key) and bumps
  `generation()`; the BuildSession's existing generation-bump path
  triggers the rebuild.
- **Open-editor conflict**: if an externally modified file has an open
  editor window with a *clean* buffer, reload silently. With a *dirty*
  buffer, show a conflict banner inside that window: `Reload (lose edits)
  / Keep my version / Show diff`.
- **Export**: **Save as archive** (writes a `.nhproj`) and **Publish package**
  (§3.4). Saving a folder-as-folder makes no sense — the folder *is* the source.
- **Close**: warn iff any editor buffer is dirty.

### 3.2 Working set (the project) — application mode

The working set is a `ZipWorkingSet` (§6.5). It backs **native archive mode** and
the **web application mode**, and it is the object every non-filesystem project
resolves to. Provenance (`Empty | Local(name) | Remote(url)`) records where it came
from and drives persistence + posture.

- **Ingestion**: dropping/picking loose files calls `add`, which `writeEntry`s them
  into the working set (bumps `generation()`). Dropping/opening a single **`.nhproj`**
  swaps the whole project to a working set opened from those bytes (`openFromBytes`).
- **Resolve**: served from the in-memory working set; entries decompress lazily on
  first `read(key)` and stay hot. A **basename-fallback** resolve policy (ported from
  the retired bag) is enabled for loose-drop provenance so flat-dropped include trees
  (`materials/common.toml` referenced, `common.toml` dropped) still link; archives
  with real paths keep strict resolution.
- **Native archive mode** (`ArchiveProjectFs`, bound to a path): `save()` serializes
  a fresh `.nhproj` and writes it atomically (temp + fsync + rename); unchanged
  entries pass through compressed via `mz_zip_writer_add_from_zip_reader`, overrides
  deflate fresh (cost ∝ archive size, not edit size). `Save as archive…` writes to a
  new path and rebinds.
- **Web application mode** (no sidecar): the working set **auto-persists to
  IndexedDB** (debounced `serialize()` → one blob under `project/<id>`), restored on
  reload — this is what makes the web app feel native (your work survives refresh).
  A single blob = one IDB transaction per save and is exactly the bytes
  `Save as archive` / `Publish` hand out — no separate serialization. The debounce
  (quiet interval or `visibilitychange`/`beforeunload`) keeps the rewrite off the
  keystroke path.
- **Editing**: per-window editor commits via `add` into the working set (native
  archive waits for a user `save()`; web app-mode auto-persists).
- **Export**: **Save as archive** (`.nhproj` download/write) and **Publish package**
  (§3.4).
- **Close / New**: clears the current IDB slot (with a confirm if dirty relative to
  the last export) and returns to an empty working set.

### 3.3 Viewer mode (web, sidecar-driven presentation)

When a **sidecar** (`nh_manifest.json`) is present, the deployment is a *publication*,
not a scratchpad:

- **Source of truth**: the sidecar names a `.nhproj` archive (URL); the viewer fetches
  those bytes once and opens a working set with provenance `Remote(url)`. The
  archive's **project manifest** (`nodehammer.toml` → `[project]`) seeds the root keys
  so the scene "just builds"; `[view]` seeds the initial steer.
- **Content locked** (a sidecar/deployment property): drops/edits are disabled, and
  the working set **never touches IDB**.
- **Reload = re-fetch from source.** Revisiting the URL always reloads the published
  archive; there is no IDB restore to override it. A published presentation reflecting
  its published source is correct.
- **Steer stays live** and is committed to the URL (§3.4) — locking content does *not*
  freeze the view; that's the whole point of a shareable posed link.
- **Editing a viewer-mode project is a deliberate, explicit step**: **Save as archive**
  downloads the `.nhproj`, which the user opens in **application mode** to edit. There
  is **no in-place fork-to-edit** (an implicit fork with a silent IDB write and unclear
  identity was rejected as confusing).

### 3.4 Steer, publishing & sharing

- **Steer** (camera / angle-cut / rotation / toggles / selected-archive) is resolved
  as a layered cascade, most-specific-wins per key:
  `app default < archive [view] < sidecar overrides < URL query`. The live steer is
  written back to the URL (debounced, continuous — extends today's `commitUrlState`),
  so the address bar always reproduces the current screen. A **sync panel** toggles
  which keys sync, continuous vs. manual ("Copy view link"), and whether URL steer is
  applied on load; its own settings persist to `localStorage`. Sharing a link fully
  reproduces the **content** only when it is URL-reachable (a sidecar/remote archive) —
  so "share my screen" is intrinsically a viewer-mode capability and closes the
  authoring→publishing loop.
- **Publish package** emits a self-contained static folder (`viewer.html` + the three
  runtime pairs + `nh_manifest.json` + `project.<hash>.nhproj`), droppable on any
  static host with zero server code (§6.6). Web app-mode publish fetches its own
  same-origin runtime siblings and zips them with the generated sidecar + archive into
  a download; native publish writes the same folder *if* the distribution ships the web
  runtime (§6.6, feasibility TBD). Content-hashed archive filename = free cache-bust.

---

## 4. State the App tracks

```
Substrate      { Filesystem, WorkingSet }     // the two backends
Provenance     { Empty, Local(name), Remote(url) }  // where the working set came from
Posture (web)  { Application, Viewer }         // derived from sidecar presence
SaveTarget     { None, ArchivePath(path) }     // native archive bound path
EditorWindow[] open_editors;                   // each owns: key, buffer, dirty bit
```

- `Substrate` is `Filesystem` (native live folder) or `WorkingSet` (everything else).
- `Provenance` replaces the old `ManifestOrigin`: it records origin and gates
  persistence — `Local` app-mode working sets auto-persist to IDB; `Remote` viewer-mode
  sets never do.
- `Posture` (web) is derived from whether a sidecar (`nh_manifest.json`) was present at
  load: it governs content-lock + IDB persistence, not the backend. Native has no
  posture (always editable, path-bound `SaveTarget`).
- `SaveTarget` lives on the App: only native archive mode has a bound path. Filesystem
  writes through individual files; the web working set persists to IDB.
- Editor windows live entirely above `ProjectFs`; they commit via `add` (see §5).

---

## 5. Simplified `ProjectFs` interface

The interface gets a little tighter. There is **no** separate `writeBytes`
entry point: `addBytes` already replaces an existing entry on
base-name collision (the bag emits a `replaced foo.toml` warning today),
which is exactly what an editor-save needs to do. Editor commits and
drag-drop ingestion both flow through the same write surface — the App
just routes drops through the `planAdd` decision and editor commits
straight to `add` (no plan / no modal, since the user already pressed
save).

The hierarchical-list change replaces today's eager-snapshot model with
a per-directory lazy one (§5.2).

### 5.1 Surface after the changes

```cpp
class ProjectFs {
public:
    virtual ~ProjectFs() = default;

    // Lifecycle
    virtual void poll() {}
    virtual ProjectFsStatus status() const = 0;
    virtual std::span<const ProjectProgress> progress() const = 0;
    virtual const std::string &errorMessage() const = 0;
    virtual std::string_view name() const = 0;
    virtual std::span<const std::string> warnings() const { return {}; }

    // Read
    virtual ResolveResult resolve(std::string_view key) const = 0;
    virtual std::uint64_t generation() const { return 0; }

    // Hierarchical listing — lazy per directory (§5.2)
    virtual std::span<const DirNode> list(std::string_view dir = {}) const = 0;
    virtual void rescan() {}                          // optional

    // Write surface (drops, picks, AND editor commits). Default reject.
    virtual ProjectDropDecision planAdd(const FileInput &) const { return reject(); }
    virtual void add(const FileInput &) {}            // replace-on-collision

    // Removal (editor delete, archive prune). Default no-op.
    virtual bool canRemove() const { return false; }
    virtual void removeKey(std::string_view key) {}
};
```

What changed from today:

- **`planAddPath` + `planAddBytes` collapsed** into one `planAdd(FileInput)`,
  where `FileInput` is a tagged variant `{Path{path} | Bytes{key, span}}`.
  Same for `addPath` + `addBytes` → `add(FileInput)`. The variant lets
  native drops carry a path (zero-copy file copy on supported platforms)
  and web drops carry bytes, without the App calling different methods
  per platform.
- **`add` is the editor commit path too.** No new method; the existing
  replace-on-collision semantics already cover edit-save. Two callers,
  one method:
  - **User drop / pick** (App-level `ingestFile`): call `planAdd` first,
    then `add` if Accept, else show the Confirm or Reject modal.
  - **Editor save** (App-level `commitEdit`): call `add` directly with
    a `Bytes{key, bytes}` payload — bypasses `planAdd` because the user
    already gave consent by pressing save.
- **Filesystem's split policy** (drops rejected, edits accepted) is
  expressed inside `planAdd`: filesystem returns Reject for inputs whose
  key doesn't already exist on disk, Accept for those that do — i.e.
  drop-onto-existing is a replace, drop-onto-new is rejected. Editor
  saves, by definition, write to keys that exist, so going through
  `add` directly does the right thing without consulting `planAdd`.
- **Added `removeKey`** for editor file deletion and archive pruning.
  Backends that don't support it leave `canRemove()` returning `false`.
- **`list(dir)` is now lazy.** See §5.2.

### 5.2 Hierarchical lazy listing

`DirNode` simplifies — no more embedded `children` span:

```cpp
struct DirNode {
    std::string name;            // last path component
    std::string key;              // logical key relative to project root
    bool is_directory{false};
    std::uint64_t bytes{0};       // 0 for directories
    // No `children` — children are obtained by calling list(key).
};
```

Contract:

- `list("")` returns the immediate children of the project root.
- `list(dir_key)` returns the immediate children of `dir_key`, where
  `dir_key` is the `key` field of some `DirNode` with `is_directory == true`.
- The returned span is valid until the next `generation()` bump. Backends
  may keep per-directory caches; spans into different directories may
  share a cache pool, but each entry's lifetime is bounded by
  `generation()`.
- Backends are free to drop unused per-dir caches at their own
  discretion (memory pressure, on directory collapse via a future
  `discardCache(dir_key)` hook). Within a single rendered frame, no span
  may be invalidated under the App's feet.

UI consequences:

- The tree panel walks visible directories via repeated `list(dir)`
  calls — typically O(visible directories × entries-per-directory) per
  frame. With per-directory caches, this is cheap after the first
  expansion.
- "Walk all UI-visible files" becomes a clean operation: traverse only
  expanded subtrees, calling `list(dir)` on each, decorating leaves with
  type icons or dirty markers without recursing into collapsed
  directories.
- Large filesystems pay no walk cost up front. Opening a 100k-file
  directory only walks the root; subdirectories walk on expand.

Backend behavior:

| Backend                       | `list("")`                                  | `list(dir)` for a non-root dir                                       |
| ----------------------------- | ------------------------------------------- | -------------------------------------------------------------------- |
| `FilesystemProjectFs`         | one `directory_iterator` over root          | one `directory_iterator` over `root / dir`                           |
| `WatchedFilesystemProjectFs`  | wraps the above, invalidates on watcher     | same, plus watcher-driven invalidation                               |
| `NativeBagProjectFs`          | iterates the storage dir's root             | iterates the storage dir's subdirectory                              |
| `ArchiveProjectFs` (web working set) | synthesized from working-set key prefixes | synthesized from key prefixes (subdirectories are virtual)     |
| `ArchiveProjectFs`            | synthesized from archive entries            | synthesized — archives carry full paths, entries grouped by prefix   |

"Flat is a special case" falls out: bag-with-flat-keys, URL-with-flat-manifest,
and archive-with-flat-keys all just have one level of children under root.

`rescan()` becomes "drop all per-dir caches and re-list lazily as the UI
re-expands." Bumps `generation()`.

### 5.3 Save / export hooks (App-level)

These are App-level operations because they target the user's filesystem, the
browser download path, or a deployable package — not the project's internal storage:

- **Create archive from scene** (landed, step 7) — promote the current project into a
  live, unbound working set (`ArchiveProjectFs`) seeded with the build closure
  (filesystem) or the whole working set (bounded backends).
- **Save as archive** — serialize the working set to a `.nhproj`: native writes to a
  picked path (and binds it); web downloads the blob.
- **Save** — native archive (bound) only — rewrites the bound `.nhproj` in place.
- **Publish package** (§3.4, §6.6) — emit the self-contained deployable folder.

There is **no** `saveAsFolder` (filesystem already *is* a folder) and **no
fork-to-edit**; editing a viewer-mode publication is Save-as-archive → open in app
mode.

---

## 6. Mapping each mode to backends

| Situation                          | Backend                                                                 |
| ---------------------------------- | ----------------------------------------------------------------------- |
| Empty (native)                     | `NativeBagProjectFs`, empty store                                       |
| Empty (web, app mode)              | `ArchiveProjectFs` (unbound working set), provenance `Empty`            |
| Filesystem (native)                | `FilesystemProjectFs`, wrapped by `WatchedFilesystemProjectFs`          |
| Open/drop a `.nhproj` (any)        | `ArchiveProjectFs` — native bound to the path; web unbound `Local`      |
| Web viewer mode (sidecar)          | `ArchiveProjectFs` from fetched archive bytes, provenance `Remote`, locked |

Notable sharing:

- `NativeBagProjectFs` and `FilesystemProjectFs` both walk a directory and resolve
  from disk (the native bag is "filesystem pointed at an app-owned dir that accepts
  ingestion").
- **The web app-mode project, web viewer-mode project, and native archive mode are all
  the same `ArchiveProjectFs` over a `ZipWorkingSet`** — they differ only in
  *provenance*, *persistence* (file path vs. IDB vs. none), and *posture* (lock). The
  old `WebBagProjectFs`, per-key `UrlProjectFs`, and standalone bag are retired.

### 6.5 The shared `ZipWorkingSet` helper

Native archive mode and the web working set (app + viewer) are all wrappers around
the same in-memory abstraction: a ZIP-backed read-on-demand store with an overlay
of edits. Pulling that out as a helper keeps the miniz integration in one place.

```cpp
class ZipWorkingSet {
public:
    // Open a ZIP from raw bytes (web: bytes from IDB or a fetched/dropped
    // archive; native: a full file read). Parses the central directory
    // immediately.
    static ZipWorkingSet openFromBytes(std::span<const std::byte>);

    // Open a ZIP from a file path (native archive). Holds the file
    // open for lazy entry reads via mz_zip_reader_init_file.
    static ZipWorkingSet openFromFile(std::filesystem::path);

    // Read on demand. Returns the original archive bytes for unchanged
    // entries (decompressed and cached on first hit) or the override
    // bytes for entries written via writeEntry. Caller's view is "no
    // distinction between original and edited" — that's the point.
    std::span<const std::byte> read(std::string_view key);

    // Override an entry. Stored in memory; archive bytes are not
    // touched until serialize().
    void writeEntry(std::string_view key, std::vector<std::byte> bytes);
    void removeEntry(std::string_view key);

    // List entries (for ProjectFs::list synthesis). Synthesizes virtual
    // directories from key prefixes.
    std::span<const DirEntry> listAtPrefix(std::string_view prefix) const;

    // Serialize current state (originals + overrides + removals) to a
    // fresh ZIP blob. Used by:
    //   - native archive Save: write blob to temp file, fsync, rename.
    //   - web app-mode persistence: write blob to IDB.
    //   - Save as archive: download (.nhproj) / write to a path.
    //   - Publish package: the project.<hash>.nhproj entry.
    std::vector<std::byte> serialize() const;

    bool dirty() const;
};
```

Properties relevant to the rest of the doc:

- **Lazy reads via miniz.** No upfront unpack. The central directory is
  small (one entry per file: name + offset + compressed size + crc).
  Entry decompression happens on `read(key)` and is cached.
- **In-place ZIP updates aren't possible.** Each `serialize()` produces
  a fresh ZIP. miniz makes "stream original entries through, swap
  modified ones" cheap by passing through the already-compressed bytes
  for unchanged entries (`mz_zip_writer_add_from_zip_reader`), so the
  cost is dominated by re-compressing the (typically small) edited
  entries plus copying the (potentially large) unchanged compressed
  bytes through. Linear in archive size, not in edit size.
- **One abstraction, three persistence policies over the same bytes.** A bound
  native `ArchiveProjectFs` calls `serialize()` on user-initiated Save → atomic
  write to its path. Web **app mode** calls `serialize()` on a debounce timer →
  IDB blob (`project/<id>`). Web **viewer mode** never persists (re-fetched from
  source). All of them, plus **Save as archive** and **Publish**, use the same
  `serialize()` byte stream — the `.nhproj`.

This means **native archive mode, web app mode, and web viewer mode share ~all of
their implementation** — one `ArchiveProjectFs` over a `ZipWorkingSet`, specialized
only by provenance/persistence/posture.

### 6.6 The publish package

**Publish package** emits a self-contained static folder — the deployable form of an
archive:

```
viewer.html                              # shell; fetches nh_manifest.json → viewer mode
nodehammer-gles3.js  / .wasm             # WebGL2 runtime
nodehammer-wgpu.js   / .wasm             # WebGPU runtime
nodehammer-compute.js/ .wasm             # compute worker
nh_manifest.json                         # sidecar: { archive: "project.<hash>.nhproj", lock, view }
project.<hash>.nhproj                    # serialized working set
```

Drop the folder on any static host and open `viewer.html` → it fetches
`nh_manifest.json` → **viewer mode** loads the archive. Zero server code. The web
build already produces the three runtime pairs (`viewer.html` picks gles3/wgpu at
runtime via `navigator.gpu`), so the payload is a known, enumerable set of files.

**Runtime source:**
- **Web (app mode):** the running app is served from `basePath`; it `fetch()`es its
  own same-origin siblings (`viewer.html` + the three `*.js`/`*.wasm` pairs), adds the
  generated sidecar + archive, and zips → download. Always version-matched.
- **Native:** the CLI writes the same folder *only if* the native distribution ships
  the web runtime (staged e.g. under `share/nodehammer/web/`). Coupling native
  packaging to a prior wasm build (build wasm → stage → include) is a **feasibility to
  establish**; until wired, native publish emits only the archive + sidecar.
- **Thin (later):** `viewer.html` gains an overridable runtime base URL so a package
  can reference a hosted, versioned, CORS-enabled runtime instead of bundling it —
  additive, needs a runtime host, out of scope now.

---

## 7. Editor windows (above `ProjectFs`)

Editor windows are an App-level component, not part of `ProjectFs`. They:

1. Read initial bytes via `App::project()->resolve(key)` and copy into a
   text buffer.
2. Track dirty per-window (independent of project-level dirty).
3. Commit by calling `App::commitEdit(key, bytes)`, which routes to
   `project_->add(FileInput{Bytes{key, bytes}})` directly — no `planAdd`
   gate (the user already gave consent by pressing save):
   - Filesystem mode: per-window `Save` (icon, ⌘S) writes through to
     disk via `add`.
   - Working-set mode: on window close (or focus loss with a dirty
     buffer) calls `commitEdit` to overwrite the entry in the working
     set (web app-mode then auto-persists to IDB; native archive waits
     for `save()`). Viewer mode is read-only — no editor commits.
4. Subscribe to `generation()` bumps to detect external changes and
   apply the conflict policy in §3.1.

One window per key (modal-per-key); reopening the same key focuses the
existing window.

---

## 8. Mode transitions in terms of `setProject`

Every transition is a `setProject` swap. The transitions and their triggers:

| Transition                       | Trigger                                                    | New backend                                                     |
| -------------------------------- | ---------------------------------------------------------- | -------------------------------------------------------------- |
| Empty → working set              | first drop / pick                                          | same working set, contents just appear (`add`)                 |
| Empty → Filesystem               | `Open folder…`                                             | `FilesystemProjectFs(path)`                                    |
| Open a `.nhproj` (native)        | `Open archive…` / drop                                     | `ArchiveProjectFs(path)` (bound)                               |
| Open a `.nhproj` (web app mode)  | `Open archive…` / drop                                     | `ArchiveProjectFs(openFromBytes)` (unbound, `Local`)          |
| Create archive from scene        | menu action (step 7)                                       | unbound `ArchiveProjectFs` seeded from the closure/whole set   |
| Filesystem/native → Archive      | `Save as archive…` (native) + optional rebind             | `ArchiveProjectFs(new path)`                                   |
| Web viewer mode (on load)        | sidecar names an archive                                   | `ArchiveProjectFs(openFromBytes(fetched))` (`Remote`, locked)  |
| Anything → Empty                 | `Close project` (with dirty confirm)                       | fresh empty working set / `makeEmptyBag()`                     |

There is **no** URL→bag graduation and **no** viewer→app fork transition anymore:
web viewer mode is a locked publication; to edit, Save-as-archive downloads the
`.nhproj` and the user opens it in app mode (a fresh `setProject` from those bytes).
There is no folder-promotion path — filesystem already is a folder.

---

## 9. UI per mode (panel + chrome)

| Mode                     | Status line                       | Toolbar actions                            | Editor save behavior                                       |
| ------------------------ | --------------------------------- | ------------------------------------------ | ---------------------------------------------------------- |
| Empty                    | "no project"                      | Open files / folder / archive              | n/a                                                        |
| Filesystem (native)      | `folder: /path/to/odd`            | Rescan, Save as archive, Publish           | per-window save (⌘S) writes through to disk                |
| Native bag               | `bag · N files · persisted`       | Add files, Create archive, Publish         | per-window edit, commit on close                           |
| Web app mode             | `project · N files` (IDB `*`)     | Add files, Save as archive, Publish        | per-window edit, auto-persists to IDB                      |
| Archive (native, bound)  | `project.nhproj *`                | Save (in-place), Save as archive, Publish  | per-window edit; `Save` flushes the working set            |
| Web viewer mode          | `viewer · <title>` (locked)       | Save as archive, Publish; **no edits**     | read-only (edit = download + open in app mode)             |

A trailing `*` denotes unsaved/uncommitted state. Per-leaf dirty markers (rendered
while walking visible `list(dir)` results) help locate edits.

---

## 10. Watcher / external-change behavior (filesystem only)

A `WatchedFilesystemProjectFs` decorator wraps `FilesystemProjectFs`. On
watcher event:

1. Modified key backs an active scene root or a transitively included
   file → invalidate the byte cache for that key, invalidate the
   per-dir lazy cache for the parent dir, bump `generation()`.
2. Modified key has an open editor window with a clean buffer → reload
   buffer silently.
3. Modified key has an open editor window with a dirty buffer → conflict
   banner inside that window.
4. Deleted key with active root → put the project into a recoverable
   error state (existing `ProjectFsStatus::Error` path); don't tear down
   the rendered scene.

The lazy-list cache integrates here: a directory event invalidates that
directory's cached entries. The UI re-fetches lazily on next render.

---

## 11. Close / quit confirmation matrix

| Mode                       | Warn on…                                                                            |
| -------------------------- | ----------------------------------------------------------------------------------- |
| Filesystem                 | any open editor with a dirty buffer                                                 |
| Native bag                 | nothing on quit (persists in the storage dir); on `Close project`, warn if any contents and offer export |
| Web app mode               | nothing on `beforeunload` — the working set is already auto-persisted to IDB and restores on reload; on `Close project` (clears the IDB slot), warn if unsaved vs. last export |
| Archive (native)           | any unsaved working-set changes (project-level dirty)                               |
| Web viewer mode            | nothing — it's a locked publication that reloads from source (no edits to lose)     |

---

## 12. Recommended build order

Each step is independently shippable and lights up real user value. Editor
work is deliberately deferred to the end — the project-organization layer
(backends, mode transitions, save/export, watcher) gets to settle first
without an editor sitting on top of churning APIs.

1. ✅ **Hierarchical lazy `list(dir)` redesign** — port the three existing
   backends (bag, filesystem, URL) onto the lazy per-dir API. Prerequisite
   for archive/bag-web's synthesized hierarchy and for the eventual
   editor's "show icons for all visible files".
2. ✅ **Introduce `ByteBuffer` and drop the filesystem byte cache** —
   landed. `ByteBuffer` (wrapping `shared_ptr<const vector<byte>>`,
   exposing `span()`/`size()`/`empty()`) lives at
   [`src/viewer/byte_buffer.hpp`](../src/viewer/byte_buffer.hpp);
   only public construction is from a `vector<std::byte>`, after which
   it's a copyable handle (refcount bump) — no `shared_ptr` is exposed
   on the API. `OpenedFile::bytes` carries it. `FilesystemProjectFs`
   reads from disk on every `resolve()` (cache + mutex deleted; OS page
   cache handles repeats); `BagProjectFs` and `UrlProjectFs` store one
   `ByteBuffer` per cached entry and hand out a copy. `BuildSession`
   holds `ByteBuffer`s in `bytes_by_key` (no immediate-copy step). The
   future editor will copy-on-write into a mutable buffer when editing
   starts, dropping the `ByteBuffer` once it owns a private copy. Locks
   in the §2 principle that "the storage layer is the cache" so when
   `NativeBagProjectFs` lands storage-backed, no separate cache layer
   needs reinventing.
3. ✅ **`NativeBagProjectFs` (storage dir + write-through `add`)** —
   landed as a thin wrapper around an inner
   [`FilesystemProjectFs`](../src/viewer/filesystem_project_fs.hpp)
   pointed at a process-owned `temp_directory_path()` subdirectory
   (created in ctor, best-effort `remove_all` in dtor). Reads delegate
   straight through to the inner FS — no separate byte cache, per the
   §2 principle. Writes (`addPath`/`addBytes`) write the file with
   `file_io::writeFile`, append/replace a `ProjectProgress` entry,
   bump a `replaced foo.toml` warning on collision, and call
   `inner_->rescan()` to invalidate the per-dir list cache and bump
   `generation()`. Subdir-key resolve falls back to the basename so
   include graphs whose siblings were dropped flat keep resolving.
   App swaps the empty bag through a `makeEmptyBag()` helper:
   native gets `NativeBagProjectFs`, web stays on the in-memory
   `BagProjectFs` until step 8.
   **Deferred from §3.2**: stable per-app data directory under
   `sago::getDataHome()`, `state.json` session-id slot for relaunch
   restore, and atomic temp+rename writes. Drops survive within a
   process (which is what step 4's watcher and step 11's editor commit
   need); cross-launch persistence and crash-safe writes follow when
   we wire up `state.json`. The persistence primitive to build this on
   already exists — `Platform::loadPersistentText`/`savePersistentText`
   plus the `app_state` viewer-state TOML round-trip (see §1) are the
   template; the bag slot just needs `getDataHome()` in place of
   `getConfigHome()` and a session-id record pointing at the active
   storage dir.
4. ✅ **`WatchedFilesystemProjectFs` decorator** + rebuild-on-change —
   landed. A pimpl decorator that wraps a `FilesystemProjectFs` and
   watches `inner->root()` via the header-only **wtr.watcher** library
   (Conan `watcher/0.14.1`, viewer-gated + native-only like
   `platform_folders`; compiled into `nodehammer_lib`). wtr delivers
   change callbacks on a background thread; the callback co-owns a
   `shared_ptr` change-state block (mutex + dirty flag) — never `this` —
   so a late callback during teardown can't touch freed memory. `poll()`
   (main thread, in the existing `onFrame` loop) debounces (default
   150 ms, coalescing an editor's truncate/write/rename burst and
   skipping half-written files) and on a settled change calls
   `inner->rescan()`. Invalidation is **coarse** — `rescan()` drops the
   whole per-dir cache and bumps `generation()`; `FilesystemProjectFs`
   exposes no finer hook and the re-walk is cheap for config trees.
   Hidden-path events (.DS_Store/.git/swap files) are filtered to match
   the inner FS's `skip_hidden_files`. `name()` forwards to inner
   (`"filesystem"`) so folder-mode UI is unchanged — watching is
   transparent. Wired at all three native filesystem-mode entry points
   (CLI folder, Open-folder picker, drag-drop folder). Watcher errors are
   stashed and surfaced as a warning from `poll()` (never from the
   background thread). A `notifyChanged()` seam makes the
   debounce/coalesce logic deterministically testable without threads;
   one `[.]`-tagged integration test exercises the real FSEvents path.
5. ✅ **`ZipWorkingSet` helper (§6.5)** — landed. miniz-backed
   ([`zip_working_set.{hpp,cpp}`](../src/viewer/zip_working_set.hpp)):
   `openFromBytes`/`openFromFile` parse the central directory into memory,
   `read(key)` decompresses lazily and caches as a `ByteBuffer`,
   `writeEntry`/`removeEntry` maintain an override+tombstone overlay,
   `listAtPrefix` synthesizes a `ZipDirEntry` tree from flat keys, and
   `serialize()` streams a fresh ZIP (unchanged entries passed through
   compressed via `mz_zip_writer_add_from_zip_reader`, overrides deflated).
   No `ProjectFs` bindings — pure substrate. miniz is Conan
   `miniz/3.0.2`, viewer-gated on **both** native and web (step 8's web
   bag reuses it), unlike the native-only watcher. Tested directly.
6. ✅ **`ArchiveProjectFs` (native)** — landed
   ([`archive_project_fs.{hpp,cpp}`](../src/viewer/archive_project_fs.hpp)).
   Wraps a `ZipWorkingSet` bound to a path; `resolve` reads from the
   working set (no basename fallback — archive keys are full paths),
   `list(dir)` synthesizes a `DirNode` tree with a per-dir cache keyed to
   `generation()` (mirrors `FilesystemProjectFs`), and drops are accepted
   as working-set overrides (Accept new / Confirm replace). `save()`
   serializes and writes atomically (temp + fsync + rename), then reopens
   from the saved bytes to drop the dirty flag. Wired into the App via
   **File → Open archive…** (native picker) and a single-`.zip` drag-drop;
   **File → Save archive** (⌘S label) is enabled when the archive is dirty.
   An integration test drives `open-archive → BuildSession → scene` headlessly.
   *Deferred*: `Save as archive…` (that's step 7).
7. ✅ **"Create archive from scene" — cross-platform archive promotion** —
   landed
   ([`archive_export.{hpp,cpp}`](../src/viewer/archive_export.hpp)).
   Instead of a one-shot save, the project is *promoted* into a live,
   unbound `ArchiveProjectFs` seeded with `buildArchiveWorkingSet` (the
   **build closure** — root config + transitive includes + geometry — for
   filesystem mode via a `ProjectFs::listingIsComplete()==false` walk that
   reuses `ConfigLoader::peekIncludesFromBytes`/`resolveIncludeKey`; the
   **whole working set** for bounded backends where `listingIsComplete()`).
   The current root keys are re-seeded so the same scene rebuilds from the
   bundle; the user then drags extra files in (archive mode accepts drops on
   both platforms) and Saves. **Archive mode is now cross-platform** (not
   native-only): only persistence differs — a **bound** archive writes in
   place (`save()`), an **unbound** one is **save-as**'d to a path (native,
   `saveTo` via NFD `SaveDialog`) or **downloaded** (web,
   `nh_viewer_download_bytes`). `ZipWorkingSet::create()` builds the
   from-scratch set; `writeBytesAtomic` is shared with step-6 `save()`.
   Tests: closure-vs-whole-set walk + unbound save/bind round-trip; verified
   on native (393 tests) and the wasm build compiles + links the whole path.
   *Deferred to R1*: opening an **existing** archive into a live web project.

### The §0 reshape (supersedes old steps 8–9)

The old steps 8 (`WebBagProjectFs`) and 9 (URL reload semantics) are replaced by the
archive-as-project reshape (§0), staged R1–R6. The detailed plan lives in the approved
plan file; the shape is:

- **R0** — this doc rewrite + locked terminology (no code).
- **R1** — Unify the web project onto the working set: generalize `ArchiveProjectFs`
  (provenance + basename-fallback resolve policy), make the web empty project a
  working set, route `.nhproj` open/drop → `ArchiveProjectFs`, enable the web
  "Open archive…" picker, switch native drop/CLI/NFD surfaces to `.nhproj`, retire the
  web `BagProjectFs`. Delivers the deferred-from-7 "open a project on web".
- **R2** — IDB persistence + application mode: an IDB EM_JS bridge + `Platform`
  `*PersistentBlob`; app-mode debounced `serialize()` → IDB, cold-load restore,
  `beforeunload` flush; restore suppressed under a sidecar.
- **R3** — Sidecar → archive + project manifest (viewer mode): repurpose
  `nh_manifest.json` to name a `.nhproj`; fetch → `ArchiveProjectFs` `Remote`, locked,
  reload-from-source; parse the archive's `nodehammer.toml` (`[project]`/`[view]`);
  retire per-key `UrlProjectFs`; no legacy compat.
- **R4** — Layered steer + URL + sync panel (§3.4).
- **R5** — Publish package (§6.6).
- **R6** *(deferred)* — named multi-document switcher; thin package runtime.

### Editor (unchanged, still last)

10. **Editor windows + per-key dirty buffers** (no save yet) —
    establishes the edit UX shape across all modes that now exist.
    Reads via `resolve()` (copying immediately, per the tightened
    lifetime), writes nowhere. Deliberately lands after all storage
    backends so the editor isn't dragged through their churn.
11. **Editor commit path: per-window ⌘S save + filesystem `add`
    policy** — `App::commitEdit` calls `add` directly (no `planAdd`
    gate, the user gave consent by pressing save). Filesystem's
    `planAdd` returns Accept only for keys that already exist on disk
    (so editor saves work, raw drops onto FS mode stay rejected). Bag
    and working-set backends commit via the same `add` surface; the bag
    overwrites in its storage dir, the working set writes its override
    and (native archive) waits for the user-initiated `Save`.

---

## 13. Decisions

These were called out as open questions during design; the choices below
are the ones we're going with.

- **Two methods for ingestion (`addPath` + `addBytes`), not a `FileInput`
  variant.** Matches today's shape. The path overload internally reads
  the file and delegates to the bytes overload, so the underlying write
  path is single. Keeps variants out of the header.
- **Editor windows are modal-per-key.** One window per file; reopening
  the same key focuses the existing window. We're not trying to support
  multiple simultaneous editor windows on the same file — see the note
  below on why that's the right choice for our setup.
- **Per-directory `list` cache lives until the next `generation()` bump,
  no collapse-driven eviction.** Caches are dropped en masse on
  generation bumps. Add eviction only if a real dataset shows memory
  pressure.
- **Tree UI state survives generation bumps.** The cache going away
  doesn't mean the UI's "expanded directories" set goes away. The tree
  panel keeps an App-side set of expanded keys; on the next render after
  a bump it re-calls `list(dir)` for each open directory and the UI
  pops back into the same shape. This means "save a file" doesn't
  collapse the tree.
- **Filesystem editor saves are manual only**, no auto-save toggle.
  Matches "explicit save = explicit rebuild" and avoids rebuild storms
  during typing.
- **The archive is the project (`.nhproj`); no back-compat.** One `ZipWorkingSet`
  substrate for authoring + publishing; a custom extension (ZIP internally). No
  `.zip` projects, no legacy per-key sidecar, `UrlProjectFs` retired.
- **Two web postures on sidecar presence; no fork-to-edit.** App mode persists to
  IDB; viewer mode is a locked publication that reloads from source. Editing a
  publication is the explicit Save-as-archive → open-in-app-mode path.
- **Content-lock is a sidecar (deployment) property; steer is never frozen.** A
  posed shareable link needs a live, URL-committed view even over locked content.
- **Single current IDB document now** (`project/<id>`), multi-document switcher later.

### Things deliberately not solved

- **Multiple OS-level viewer windows on the same project.** ImGui is
  immediate-mode and the app is one window per process. We treat the
  viewer as single-window-per-machine and enforce that at the OS level
  rather than in the data layer — see
  [viewer-single-instance.md](viewer-single-instance.md) for the plan
  (lockfile + IPC forward for terminal invocations, macOS `.app`
  bundle with `LSMultipleInstancesProhibited` for GUI invocations).
  Once that ships, "single window per project" is an enforced invariant
  rather than a convention; if multi-window ever becomes important,
  revisit by introducing an explicit document layer (`NSDocument` +
  `NSWindowController` is the macOS-native shape) rather than
  retrofitting one.
- **Multiple browser tabs on the same web project.** IDB is origin-scoped and shared
  across tabs, so tabs can't be neatly keyed apart. Decision:
  - **Viewer mode (sidecar) never persists to IDB at all** — the published archive is
    the source of truth on reload. To keep edits, Save-as-archive and open the
    `.nhproj` in application mode.
  - **Application mode** persists to a single current IDB slot (`project/<id>`). Last
    writer wins; a console warning fires if a `storage` event from another tab
    indicates a concurrent writer. Multi-tab is a degenerate case for now; a future
    Web Locks API single-threading is possible but off the critical path. The named
    multi-document switcher (R6) would give explicit per-document slots.
