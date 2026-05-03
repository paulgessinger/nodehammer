#pragma once

#include <nodehammer/viewer/project_fs.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

namespace nodehammer::viewer {

/// Native bag backend: write-through bag whose storage is a real on-disk
/// directory, served by an inner `FilesystemProjectFs`. Replaces the
/// in-memory `BagProjectFs` on native builds; web continues to use
/// `BagProjectFs` until `WebBagProjectFs` lands (strategy doc step 8).
///
/// Stage 3 of the project-FS migration: drops persist as files in a
/// process-owned directory under `temp_directory_path()`. Persistence
/// across app launches is intentionally deferred — the storage dir is
/// allocated in the ctor and best-effort removed in the dtor. A future
/// stage will swap the temp dir for a stable per-app data directory plus
/// a `state.json` slot to remember the active bag across launches.
///
/// Reads delegate to the inner `FilesystemProjectFs` so the OS page
/// cache is the only byte cache (per the §2 "storage layer is the
/// cache" principle). Writes (`addPath`/`addBytes`) write a file to the
/// storage dir, append/replace a `ProjectProgress` entry for the UI,
/// and call `inner.rescan()` to invalidate the per-dir listing cache
/// and bump `generation()`. Replace-on-collision matches the in-memory
/// bag: a second drop of the same basename overwrites the first and
/// emits a `replaced foo.toml` note via `warnings()`.
///
/// `resolve(key)` first delegates to the inner FS; if the exact key is
/// missing and the key contains a slash, it retries with the basename
/// to preserve the in-memory bag's "subdir reference resolves to flat
/// drop" fallback for include graphs whose siblings were dropped flat.
class NativeBagProjectFs final : public ProjectFs {
  public:
    NativeBagProjectFs();
    ~NativeBagProjectFs() override;

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    std::string_view name() const override { return "bag"; }
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

    /// Storage directory exposed for tests; otherwise opaque. The dir is
    /// owned by the bag's lifetime — do not write into it from outside.
    const std::filesystem::path &storageDir() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
