# Viewer project + edit + save strategy

This document describes the target interaction model for the viewer's project
system across its current and planned modes (URL, bag, filesystem, archive),
and connects that model to the existing
[`ProjectFs`](../include/nodehammer/viewer/project_fs.hpp) abstraction —
including which capabilities exist today, which need additions to (or
simplifications of) the base interface, and which are best expressed as new
concrete backends or decorators.

---

## 1. Where we are today

The viewer already has the right shape for this work; what's missing is mostly
a write path, a unified hierarchical listing model, an editor surface, and a
few mode-transition rules.

**Status:** Steps 1, 2, 3, and 4 of §12 have landed.
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
- [`BuildSession`](../include/nodehammer/viewer/build_session.hpp) reads
  `generation()` and re-walks on bump or `force_walk`. Any mutation that
  bumps `generation()` (write-through to disk, watcher reload, archive
  remount) already flows through the existing rebuild path.
- The newly hoisted polling loop in `App::Impl::onFrame` runs the
  pipeline regardless of whether a scene is loaded, so file additions and
  edits drive a rebuild even after the first scene renders.
- A platform persistent-text primitive is in place:
  [`Platform::loadPersistentText` / `savePersistentText`](../include/nodehammer/viewer/platform.hpp)
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
2. **Storage is platform-native.** Bag mode is not "in-memory" any more —
   on native it persists into the app data folder, on web into IndexedDB.
   `resolve` reads from that storage. Persistence is a property of the
   storage backend, not a separate concern bolted on top.
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
- **Save target for export**: only **archive export** (`Save as archive…`).
  Saving a folder-as-folder makes no sense for filesystem mode — the
  folder *is* the source. After exporting an archive, offer
  "Reopen as archive?" the same way bag mode does (§3.2).
- **Close**: warn iff any editor buffer is dirty.

### 3.2 Bag mode

Bag mode is **split per platform** because its storage is platform-specific.
Both implementations share the `ProjectFs` interface and the same App-side
behavior; only the backing store differs.

#### `NativeBagProjectFs` (native)

- **Storage**: a per-app data directory (e.g.
  `~/Library/Application Support/nodehammer/bag/<session-id>/` on macOS,
  XDG-equivalent elsewhere).
- **Drops / picks**: `addPath` copies the source file into the storage
  dir (atomic temp + rename); `addBytes` writes a new file into the
  storage dir. Both bump `generation()`.
- **Resolve**: opens the file from the storage dir on every call. Same
  shape as `FilesystemProjectFs` — no in-memory byte cache; the OS page
  cache handles repeats, and the BuildSession / editor copy bytes into
  their own buffers anyway. They share most of their resolve/list
  implementation.
- **Persistence**: by virtue of writing through, the bag survives across
  app launches. A small session-id slot in `~/.../nodehammer/state.json`
  remembers which storage dir is the "current bag" so relaunch can
  restore it.
- **Editing**: per-window editor, no save icon. On window close (or
  focus loss with a dirty buffer), the editor calls `writeBytes` and the
  bag overwrites the underlying file. Edits are persisted by the same
  write-through.

#### `WebBagProjectFs` (web)

- **Storage**: a single ZIP blob held in memory as the live working set,
  persisted to IndexedDB as one (or a small number of chunked) blob
  records. See §6.5 for the shared `ZipWorkingSet` helper that backs
  this — the web bag is structurally the same as native archive mode,
  just persisted to IDB instead of to a file path.
- **Drops / picks**: `add(FileInput)` decompresses the bytes (if a path)
  or takes the bytes directly, writes them into the in-memory working
  set, and schedules a debounced flush of the ZIP blob to IDB. While
  the IDB flush is in flight, `progress()` reports it; reads remain
  served from the in-memory working set, so `resolve` stays synchronous
  Ready (the Pending shape is only needed during the initial IDB load
  on session startup).
- **Resolve**: serves from the in-memory working set after startup. On
  cold load, the backend reads the ZIP blob from IDB and parses its
  central directory; entries are decompressed lazily on first
  `resolve(key)` and kept hot.
- **Persistence**: IDB keeps the ZIP blob across reloads, **except**
  when reloading from a manifest URL (see §3.4 — manifest origin
  overrides IDB restore).
- **Editing**: same per-window editor; commit writes into the working
  set via `add` and triggers the debounced IDB flush.
- **Why ZIP, not per-key records?** A single blob means one IDB
  transaction per save instead of N (lower per-record overhead, simpler
  atomicity), and the same blob is what `Save → Download archive`
  hands to the browser — no separate serialization step. The trade-off
  is that every persisted save rewrites the whole blob; a debounce
  (commit + N seconds quiet, or on `visibilitychange`) keeps that off
  the keystroke path.

#### Save semantics (both)

- **No "save individual file" action** — there's nothing to save to
  externally. Saving in-app already happens (it's just write-through).
- **Save = archive export.**
  - Native: writes a `.zip` to a chosen path. Optional follow-up modal
    "Reopen from archive?" — accepting swaps to archive mode pointed at
    the new file.
  - Web: triggers a browser download of the archive blob. No graduation
    is possible (no path to bind to).
- **Close project**: clears the storage dir / IDB entries (or prompts if
  bag has any contents).

### 3.3 Archive mode

#### Native (`ArchiveProjectFs`)

- **Source of truth**: the archive file on disk. The backend uses miniz
  to open the file (`mz_zip_reader_init_file`, or load to memory and use
  `mz_zip_reader_init_mem` for small archives), parse the central
  directory once, and decompress entries lazily on first `resolve(key)`.
  Decompressed bytes are cached. **No upfront unpack to a temp dir** —
  reads come straight out of the ZIP.
- **Working set**: see §6.5. Edits are tracked as overrides over the
  read-only archive view; `add(key, bytes)` puts the new bytes into the
  in-memory override map. Subsequent reads prefer the override.
- **`Save` is a full rewrite, not an in-place edit.** ZIP can't update
  an entry in place except in the degenerate "exact-same compressed
  size" case — any size change shifts every subsequent entry's offset.
  The save path is therefore: open a writer, stream every entry from
  the reader (passing through the original compressed bytes for
  unchanged entries via miniz's reader-to-writer path; deflating fresh
  for overrides), finalize, atomic temp + fsync + rename. Cost is
  proportional to total archive size, not edit size — fine for typical
  config-sized projects.
- **`Save as archive…`**: same write path, different destination;
  optional rebind via "Reopen from archive?".
- **External archive changes**: out of scope (archives are app-owned
  while open).

#### Web

- Opening / dropping a `.zip` extracts entries into the **WebBagProjectFs**
  (no live archive mode on web). From there, bag rules apply: edit,
  export download. The graduation is automatic and shown as a toast.

### 3.4 URL session (web only)

- **Source of truth**: the URL manifest, every time the page loads.
- **Editing**: allowed. First edit (or first drop) graduates the project
  to a `WebBagProjectFs` populated from the URL session's already-fetched
  bytes. The graduation is an `App::setProject(...)` swap. The original
  manifest URL is **remembered** as session metadata (`ManifestOrigin`).
- **Reload semantics** (key constraint):
  - Reload (or revisit the manifest URL) **always** reloads the manifest
    from scratch. The IndexedDB-persisted bag is **not** restored over a
    manifest load — the manifest wins.
  - On tab close / `beforeunload` after first edit / graduation, show a
    browser confirm: "Edits in this session won't survive reload —
    export as archive first?"
  - The IDB bag-persistence slot is **scoped to "no manifest" sessions**;
    manifest-launched sessions never read from it on load. (Optionally:
    write to a separate slot keyed by manifest URL with a clear
    "not auto-restored" semantic, accessible only via an explicit user
    action.)
- **Save**: same as web bag once graduated — archive download.
- **Close**: clears manifest-scoped persisted state.

---

## 4. State the App tracks

```
ProjectMode    { Empty, Url, Bag, Filesystem, Archive }
SaveTarget     { None, ArchivePath(path) }    // path-like target for native Save
ManifestOrigin { absent, present(url) }       // sticky for the session lifetime
EditorWindow[] open_editors;                  // each owns: key, buffer, dirty bit
```

- `ProjectMode` is derived from the current `project_->name()` plus a small
  graduated-from sidecar.
- `SaveTarget` lives on the App, not on `ProjectFs`. Filesystem and bag
  modes have no path target (FS writes through individual files; bag
  writes through to its storage). Archive mode has the bound archive
  path. Everything else has no target.
- `ManifestOrigin` is read-only context that survives graduation; it
  gates reload semantics on web only.
- Editor windows live entirely above `ProjectFs`; they call
  `writeBytes` to commit (see §5).

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
| `WebBagProjectFs`             | synthesized from key prefixes in IDB        | synthesized from key prefixes (subdirectories are virtual)           |
| `UrlProjectFs`                | synthesized from manifest keys              | synthesized — most manifests are flat (root only)                    |
| `ArchiveProjectFs`            | synthesized from archive entries            | synthesized — archives carry full paths, entries grouped by prefix   |

"Flat is a special case" falls out: bag-with-flat-keys, URL-with-flat-manifest,
and archive-with-flat-keys all just have one level of children under root.

`rescan()` becomes "drop all per-dir caches and re-list lazily as the UI
re-expands." Bumps `generation()`.

### 5.3 Save / export hooks (App-level)

These are App-level operations because they target the user's filesystem
or the browser download path, not the project's internal storage:

- `App::saveAsArchive(path | downloadName)` — walks the project via
  `list()` + `resolve()`, zips into the chosen path (native) or
  triggers a download (web). Available in all modes that have content
  to export.
- `App::saveActiveArchive()` — archive (native) only — rewrites the
  bound archive path in place.

After `saveAsArchive` finishes:

- **Native, in any mode**: offer a "Reopen as archive?" modal. Accepting
  swaps to `ArchiveProjectFs` pointed at the new file. This makes the
  promotion symmetric for filesystem, bag, and archive (re-export).
- **Web**: no follow-up — there's no path to bind to.

There is **no** `saveAsFolder`. Filesystem mode already *is* a folder;
bag-as-folder is what bag native already does internally; for export
we use archive only.

---

## 6. Mapping each mode to backends

| Mode                  | Backend                                                                  |
| --------------------- | ------------------------------------------------------------------------ |
| Empty                 | `NativeBagProjectFs` (native) or `WebBagProjectFs` (web), with empty store |
| URL session           | `UrlProjectFs`                                                           |
| URL → graduated (web) | `WebBagProjectFs` populated from URL bytes, carries `ManifestOrigin`     |
| Bag (native)          | `NativeBagProjectFs`                                                     |
| Bag (web)             | `WebBagProjectFs`                                                        |
| Filesystem            | `FilesystemProjectFs`, later wrapped by `WatchedFilesystemProjectFs`     |
| Archive (native)      | `ArchiveProjectFs`                                                       |
| Archive (web) on open | `WebBagProjectFs` populated from archive entries (no live archive mode)  |

Notable sharing:

- `NativeBagProjectFs` and `FilesystemProjectFs` both walk a directory
  and resolve from disk. The bag is essentially "filesystem mode pointed
  at an app-owned directory that accepts ingestion." Share an internal
  helper or implement bag in terms of filesystem with an ingestion hook.
- `ArchiveProjectFs` and `WebBagProjectFs` both have a live in-memory
  ZIP working set and persist that ZIP somewhere (file path vs. IDB
  blob). They share the helper described in §6.5; the only thing each
  one specializes is **persistence**.

### 6.5 The shared `ZipWorkingSet` helper

Both archive (native) and the web bag are wrappers around the same
in-memory abstraction: a ZIP-backed read-on-demand store with an
overlay of edits. Pulling that out as a helper avoids duplicating the
miniz integration twice.

```cpp
class ZipWorkingSet {
public:
    // Open a ZIP from raw bytes (web bag: bytes from IDB; archive:
    // bytes from a memory-mapped file or full read). Parses the
    // central directory immediately.
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
    //   - web bag persistence: write blob to IDB.
    //   - web bag download: hand blob to the browser.
    //   - native bag/FS "Save as archive": same path.
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
- **One abstraction, two persistence policies.** `ArchiveProjectFs`
  calls `serialize()` on user-initiated Save and atomically writes the
  result to its bound path. `WebBagProjectFs` calls `serialize()` on a
  debounce timer and writes the result into IDB. Both also use
  `serialize()` for the "Save as archive / Download archive" export —
  it's literally the same byte stream.

This means **archive (native) and bag (web) share ~all of their
implementation**, and the build order in §12 collapses two of its steps
(see updated §12).

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
   - Bag / archive mode: on window close (or focus loss with a dirty
     buffer) calls `commitEdit` to overwrite the entry in the bag /
     archive working set.
4. Subscribe to `generation()` bumps to detect external changes and
   apply the conflict policy in §3.1.

One window per key (modal-per-key); reopening the same key focuses the
existing window.

---

## 8. Mode transitions in terms of `setProject`

Every transition is a `setProject` swap. The transitions and their triggers:

| Transition                       | Trigger                                                                                  | New backend                                                             |
| -------------------------------- | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------- |
| Empty → Bag                      | first drop / pick                                                                        | (already happens — same `*BagProjectFs` instance, contents just appear) |
| Empty → Filesystem               | `Open folder…`                                                                           | `FilesystemProjectFs(path)`                                             |
| Empty → URL                      | manifest URL on page load                                                                | `UrlProjectFs` (web entry point)                                        |
| Empty → Archive (native)         | `Open archive…`                                                                          | `ArchiveProjectFs(path)`                                                |
| URL → Bag (graduation)           | first edit, drop, or `add` after URL load                                                | `WebBagProjectFs` populated with URL-fetched bytes; `ManifestOrigin` preserved |
| Bag (native) → Archive           | `Save as archive…` + user accepts "Reopen as archive?"                                   | `ArchiveProjectFs(new path)`                                            |
| Filesystem → Archive             | `Save as archive…` + user accepts "Reopen as archive?"                                   | `ArchiveProjectFs(new path)`                                            |
| Archive → Archive                | `Save as archive…` to a different path + user accepts "Reopen as archive?"               | `ArchiveProjectFs(new path)`                                            |
| Archive (web) on open            | dropping or opening a `.zip`                                                             | `WebBagProjectFs` populated from archive entries                        |
| Anything → Empty                 | `Close project` (with dirty confirm)                                                     | fresh empty `*BagProjectFs`                                             |

Note the symmetry: every native mode can promote to archive via the same
"Save as archive → Reopen?" pattern. There is no folder-promotion path —
filesystem already is a folder, bag native is a folder by virtue of its
storage dir.

---

## 9. UI per mode (panel + chrome)

| Mode                | Status line                            | Toolbar actions                                | Editor save behavior                                                |
| ------------------- | -------------------------------------- | ---------------------------------------------- | ------------------------------------------------------------------- |
| Empty               | "no project"                           | Open files / folder / archive                  | n/a                                                                 |
| Filesystem          | `folder: /path/to/odd`                 | Rescan, Save as archive                        | per-window save (⌘S) writes through to disk                         |
| Bag (native)        | `bag · N files · persisted`            | Add files, Save as archive                     | per-window edit, no save icon (commit on close)                     |
| Bag (web)           | `bag · N files`                        | Add files, Download archive                    | per-window edit, no save icon (commit on close)                     |
| Archive (native)    | `archive: bundle.zip *`                | Save (in-place), Save as archive               | per-window edit; archive `Save` flushes the working set             |
| URL                 | `url session · manifest: …`            | Download archive, Disconnect from manifest     | per-window edit; first edit silently graduates to bag-with-manifest |

A trailing `*` denotes overall project dirty state. Per-leaf dirty markers
(rendered while walking visible `list(dir)` results) help locate edits.

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
| Bag (native)               | nothing on quit (state persists in the storage dir); on `Close project`, warn if any contents and offer export |
| Bag (web)                  | `beforeunload` if any edits since session start (storage persists across reloads, but explicit close clears it) |
| Archive (native)           | any unsaved working-set changes (project-level dirty)                               |
| URL (un-edited)            | nothing                                                                             |
| URL (post-edit / graduated)| `beforeunload` warning that reload restores the manifest                            |

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
   [`include/nodehammer/viewer/byte_buffer.hpp`](../include/nodehammer/viewer/byte_buffer.hpp);
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
   [`FilesystemProjectFs`](../include/nodehammer/viewer/filesystem_project_fs.hpp)
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
5. **`ZipWorkingSet` helper (§6.5)** — miniz integration: open from
   bytes / open from file path, lazy `read(key)`, in-memory overrides
   via `writeEntry`, `serialize()` for save/export. Keeps decompressed
   entries hot (decompression cost, not IO cost, is what justifies the
   cache here — consistent with the §2 principle). No `ProjectFs`
   bindings yet — this is just the substrate. Tested directly.
6. **`ArchiveProjectFs` (native)** — thin wrapper around `ZipWorkingSet`
   opened from a file path. `Save` calls `serialize()` and writes
   atomically (temp + fsync + rename). `Save as archive…` is the same
   path with a different destination.
7. **`App::saveAsArchive` for non-archive modes** — walks the project
   via `list()`/`resolve()` into a fresh `ZipWorkingSet`, calls
   `serialize()`, and writes natively or triggers a browser download.
   "Reopen as archive?" follow-up on native (works from filesystem,
   bag, and archive — symmetric).
8. **`WebBagProjectFs` (ZIP-in-IDB)** — wraps `ZipWorkingSet` opened
   from bytes loaded out of IDB. `add` goes to `writeEntry`; debounced
   `serialize()` writes the blob back to IDB. "Download archive" reuses
   the same blob — no separate path. Replaces today's web bag flow
   entirely.
9. **URL manifest reload semantics + `beforeunload` warning** — lands
    alongside web bag persistence (they share IDB scoping logic).
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
    and archive backends commit via the same `add` surface; the bag
    overwrites in its storage dir, archive writes to its working set
    and waits for the user-initiated `Save`.

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
- **URL post-graduation breadcrumb stays visible** in the status line
  ("graduated from: …") so reload semantics are understandable.

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
- **Multiple browser tabs on the same web project.** The same manifest
  URL (or no-manifest bag) opened in two tabs cannot be neatly keyed
  apart — IDB is origin-scoped and shared across tabs. This means a
  per-URL IDB slot wouldn't actually disambiguate two tabs on the same
  URL, and the "default no-manifest bag" slot likewise races between
  tabs. Decision:
  - Manifest-launched sessions **don't persist to IDB at all.** The
    manifest is the source of truth on reload. If the user wants to
    keep edits across reload, the answer is "Download archive, then
    reopen the archive" — that gives them a regular bag on next visit.
  - No-manifest bag sessions persist to a single global IDB slot. Last
    writer wins; a console warning fires if a `storage` event from
    another tab indicates a concurrent writer. We accept multi-tab as
    a degenerate case for now. A future improvement could use the Web
    Locks API to single-thread persistence to the active tab, but it's
    not on the critical path.
