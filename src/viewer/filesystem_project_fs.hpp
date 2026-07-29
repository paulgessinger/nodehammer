#pragma once

#include <viewer/project_fs.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace nodehammer::viewer {

/// ProjectFs backend that mounts a real on-disk directory and serves its
/// files lazily by relative-to-root keys. Native-only: filesystem access
/// has no meaning under the web sandbox, so this header is included only
/// in the non-emscripten branch of `nh_add_viewer_lib`.
///
/// Construction walks the directory once with `recursive_directory_iterator`
/// and builds a flat `DirNode` snapshot exposed via `list()`. Bytes are
/// read on first `resolve(key)` and cached in memory; subsequent resolves
/// return spans into the cache. `rescan()` drops the cache and re-walks
/// the tree, bumping `generation()` so the BuildSession re-walks include
/// graphs against fresh bytes.
///
/// `addPath` / `addBytes` are intentional no-ops: this backend is rooted
/// at a directory on disk, so out-of-band drops would need an overlay to
/// land coherently. The no-op surfaces a `warnings()` entry pointing the
/// user at Rescan / on-disk edits; Stage 6's overlay decorator will
/// subsume the case cleanly.
class FilesystemProjectFs final : public ProjectFs {
  public:
    /// Options for the directory walk. `skip_hidden_files` (default
    /// true) drops any entry whose name starts with `.` — common
    /// noise like `.DS_Store`, `.git`, editor swap files. Toggle off
    /// for the rare case where a project intentionally uses dot-
    /// prefixed includes.
    struct Options {
        bool skip_hidden_files{true};
    };

    /// `root` is canonicalised to absolute on construction. Throws
    /// `std::filesystem::filesystem_error` if the directory cannot be
    /// walked (does-not-exist, permission denied, not a directory).
    /// Two overloads instead of `Options options = {}` because
    /// default arguments are parsed at the end of the outer class
    /// definition, but Options's in-class member initializer for
    /// `skip_hidden_files` is also parsed there — using `Options{}`
    /// as a default arg would race itself.
    explicit FilesystemProjectFs(const std::filesystem::path &root);
    FilesystemProjectFs(const std::filesystem::path &root, Options options);
    ~FilesystemProjectFs() override;

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    std::string_view name() const override { return "filesystem"; }
    std::span<const std::string> warnings() const override;

    ProjectDropDecision planAddPath(const std::filesystem::path &path) const override;
    ProjectDropDecision planAddBytes(std::string_view filename,
                                     std::span<const std::byte> bytes) const override;
    void addPath(const std::filesystem::path &path) override;
    void addBytes(std::string_view filename, std::span<const std::byte> bytes) override;

    ResolveResult resolve(std::string_view key) const override;
    std::uint64_t generation() const override;

    std::span<const DirNode> list(std::string_view dir = {}) const override;
    void rescan() override;

    /// Mount root, canonical absolute path.
    const std::filesystem::path &root() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
