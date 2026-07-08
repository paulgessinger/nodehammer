#pragma once

#include <nodehammer/viewer/project_fs.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

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
/// Native-only: the web build extracts archives into the bag instead of opening
/// a live archive mode.
class ArchiveProjectFs final : public ProjectFs {
  public:
    /// Open the archive at `path`. Does not throw on a bad archive: the backend
    /// enters `ProjectFsStatus::Error` and `errorMessage()` describes it, so the
    /// App can install it and surface the failure like any other backend.
    explicit ArchiveProjectFs(std::filesystem::path path);
    ~ArchiveProjectFs() override;

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    std::string_view name() const override { return "archive"; }
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

    /// The bound archive path (the `Save` target).
    const std::filesystem::path &path() const;

    /// True if there are unsaved working-set edits.
    bool dirty() const;

    /// Serialize the working set and write it atomically to the bound path.
    /// Returns false (and pushes a warning) on failure; clears the dirty flag on
    /// success.
    bool save();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
