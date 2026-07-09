#pragma once

#include <nodehammer/viewer/filesystem_project_fs.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

/// ProjectFs decorator that wraps a `FilesystemProjectFs` and watches its
/// mount root for on-disk changes (via the header-only wtr.watcher
/// library). On a debounced change it calls `inner->rescan()`, bumping
/// `generation()` so the BuildSession re-walks the include graph against
/// fresh bytes — the canonical edit-on-disk → viewer-rebuild loop
/// (strategy doc step 4).
///
/// Native-only: both the wtr.watcher dependency and `FilesystemProjectFs`
/// are compiled only in non-emscripten builds, and the dep is gated on the
/// Conan `viewer` option (like platform_folders).
///
/// All reads/listing/ingestion forward to the inner FS; `name()` forwards
/// too, so the App's folder-mode detection and "folder: …" UI are
/// unchanged — watching is transparent.
class WatchedFilesystemProjectFs final : public ProjectFs {
  public:
    /// Takes ownership of an already-constructed inner FS and begins
    /// watching `inner->root()` immediately. `debounce` coalesces bursts
    /// of filesystem events (an editor's truncate/write/rename save fires
    /// several) into a single `rescan`, and avoids rebuilding a
    /// half-written file: a change fires only once the watch has been
    /// quiet for `debounce`.
    explicit WatchedFilesystemProjectFs(
        std::unique_ptr<FilesystemProjectFs> inner,
        std::chrono::milliseconds debounce = std::chrono::milliseconds{150});
    ~WatchedFilesystemProjectFs() override;

    // Lifecycle
    void poll() override;
    void setLogSink(LogSink *sink) noexcept override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    std::string_view name() const override;
    bool listingIsComplete() const override;
    std::span<const std::string> warnings() const override;

    // Ingestion — forwarded to inner.
    ProjectDropDecision planAddPath(const std::filesystem::path &path) const override;
    ProjectDropDecision planAddBytes(std::string_view filename,
                                     std::span<const std::byte> bytes) const override;
    void addPath(const std::filesystem::path &path) override;
    void addBytes(std::string_view filename, std::span<const std::byte> bytes) override;

    // Read / listing — forwarded to inner.
    ResolveResult resolve(std::string_view key) const override;
    std::uint64_t generation() const override;
    std::span<const DirNode> list(std::string_view dir = {}) const override;
    void rescan() override;

    /// Mark the watched tree dirty. Normally called by the OS watch
    /// callback (after filtering); also the deterministic test seam — with
    /// a zero debounce, a `notifyChanged()` + `poll()` pair produces
    /// exactly one `generation()` bump without involving the OS watcher.
    void notifyChanged();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
