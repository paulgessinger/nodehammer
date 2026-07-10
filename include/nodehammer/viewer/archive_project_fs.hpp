#pragma once

#include <nodehammer/viewer/project_fs.hpp>
#include <nodehammer/viewer/zip_working_set.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace nodehammer::viewer {

/// Native archive backend (strategy doc step 6): a `ProjectFs` over a `.zip`
/// project bound to a path on disk. Wraps a `ZipWorkingSet` — reads come
/// straight out of the ZIP (decompressed lazily, cached), and edits are tracked
/// as in-memory overrides over the read-only archive view. Nothing touches the
/// file until `save()`, which serializes a fresh ZIP and writes it atomically
/// (temp + fsync + rename) to the bound path.
///
/// Archive keys carry full forward-slash paths, so `list(dir)` synthesizes a
/// hierarchy from key prefixes with a per-directory cache keyed to
/// `generation()`, mirroring `FilesystemProjectFs`. Unlike the bag there is no
/// basename fallback in `resolve()` — archive keys are already full paths.
///
/// Ingestion is accepted (unlike the read-only filesystem backend): a drop of a
/// new basename is `Accept`, a drop onto an existing key is `Confirm` (replace).
/// Committed drops become working-set overrides and mark the project dirty; the
/// user persists them via `save()`. External changes to the archive file while
/// it is open are out of scope (the archive is app-owned while open).
///
/// Archive mode is cross-platform: the backend and its `ZipWorkingSet` are
/// viewer-gated on both native and web. Only *persistence* differs — a bound
/// archive (constructed from a path) writes in place via `save()`; an unbound
/// archive (constructed from a working set, e.g. "Create archive from scene")
/// has no path, so it is either save-as'd to a path (native) or downloaded as a
/// blob (web). The POSIX file-write internals are the only native-only piece.
class ArchiveProjectFs final : public ProjectFs {
  public:
    /// Where the working set came from. Drives the App's persistence/posture
    /// choices and the `resolve()` basename-fallback policy:
    ///   - `Empty`  — a scratch working set (started empty, may accumulate flat
    ///                loose drops). Basename-fallback resolve is ON so an include
    ///                like `sub/common.toml` still links to a root-dropped
    ///                `common.toml`. Web app mode persists these to IDB.
    ///   - `Local`  — opened from a real `.nhproj` (bound path, dropped/picked
    ///                bytes, or "create archive from scene"). Full paths, strict
    ///                resolve.
    ///   - `Remote` — fetched from a URL (web viewer mode). Full paths, strict
    ///                resolve; the App keeps it locked and never persists it.
    enum class Provenance { Empty, Local, Remote };

    /// Open the archive at `path` (bound mode, `Local`). Does not throw on a bad
    /// archive: the backend enters `ProjectFsStatus::Error` and the message
    /// describes it, so the App can install it and surface the failure like any
    /// other backend.
    explicit ArchiveProjectFs(std::filesystem::path path);

    /// Wrap an in-memory working set (unbound mode): no backing file yet. Ready
    /// immediately; `save()` fails until a path is bound (native `saveTo`), and
    /// `serialize()` feeds the web download path. `provenance` defaults to `Local`
    /// (a real curated set, e.g. create-from-scene or an opened `.nhproj`); pass
    /// `Empty` for a scratch web project and `Remote` for a fetched viewer-mode one.
    explicit ArchiveProjectFs(ZipWorkingSet ws, Provenance provenance = Provenance::Local);

    ~ArchiveProjectFs() override;

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    std::string_view name() const override { return "archive"; }
    bool listingIsComplete() const override { return true; }
    std::span<const std::string> warnings() const override;
    ResolveResult resolve(std::string_view key) const override;
    std::uint64_t generation() const override;
    std::span<const DirNode> list(std::string_view dir = {}) const override;
    void rescan() override;

    ProjectDropDecision planAddPath(const std::filesystem::path &path) const override;
    ProjectDropDecision planAddBytes(std::string_view filename,
                                     std::span<const std::byte> bytes) const override;
    void addPath(const std::filesystem::path &path) override;
    void addBytes(std::string_view filename, std::span<const std::byte> bytes) override;
    ProjectDropDecision planRemove(std::string_view key) const override;
    void removeKey(std::string_view key) override;
    ProjectDropDecision planMove(std::string_view from_key, std::string_view to_key) const override;
    void moveKey(std::string_view from_key, std::string_view to_key) override;

    /// Where the working set came from (persistence/posture + resolve policy).
    Provenance provenance() const;

    /// The bound archive path (the `Save` target); empty in unbound mode.
    const std::filesystem::path &path() const;

    /// True once the archive is bound to a filesystem path (path ctor, or after a
    /// successful `saveTo`). Unbound archives must be save-as'd (native) or
    /// downloaded (web).
    bool isBound() const;

    /// True if there are unsaved working-set edits.
    bool dirty() const;

    /// The current effective state serialized to a fresh ZIP blob (originals −
    /// removals + overrides). Feeds the web "Download archive" path.
    std::vector<std::byte> serialize() const;

    /// Serialize the working set and write it atomically to the bound path.
    /// Returns false (and pushes a warning) on failure; clears the dirty flag on
    /// success. In unbound mode there is no path, so this fails — the App routes
    /// unbound saves through `saveTo` (native save-as) or `serialize` (web).
    bool save();

#ifndef __EMSCRIPTEN__
    /// Serialize and write atomically to `path`, binding the archive to it (so
    /// subsequent `save()`s write in place) and clearing the dirty flag. Native
    /// only — web has no filesystem path to bind.
    bool saveTo(const std::filesystem::path &path);
#endif

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
