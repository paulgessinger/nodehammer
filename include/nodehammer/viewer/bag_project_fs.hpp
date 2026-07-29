#pragma once

#include <nodehammer/viewer/project_fs.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace nodehammer::viewer {

/// ProjectFs backend that accumulates files from drag-drop and file-picker
/// gestures. Long-lived (App-owned, not per-gesture), so dropping a config
/// in one gesture and a geometry in the next adds to the same bag rather
/// than replacing it.
///
/// Stage 2: bag is a pure key→bytes store. It has no awareness of file
/// types — recognition lives one layer up (the App scans `progress()` to
/// classify entries). Bytes are held in memory keyed by case-folded
/// filename; subdir-collapsing fallback in `resolve()` handles includes
/// like `subdir/foo.toml` whose siblings were dropped flat.
///
/// Last-write wins on basename collisions: dropping a second `foo.toml`
/// replaces the first and emits a `replaced foo.toml` note via
/// `warnings()`.
class BagProjectFs final : public ProjectFs {
  public:
    BagProjectFs();
    ~BagProjectFs() override;

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    std::string_view name() const override { return "bag"; }
    bool listingIsComplete() const override { return true; }
    std::span<const std::string> warnings() const override;
    ResolveResult resolve(std::string_view key) const override;
    std::uint64_t generation() const override;
    std::span<const DirNode> list(std::string_view dir = {}) const override;

    ProjectDropDecision planAddPath(const std::filesystem::path &path) const override;
    ProjectDropDecision planAddBytes(std::string_view filename,
                                     std::span<const std::byte> bytes) const override;

    /// Add a file by path: read its bytes into the bag and key by the
    /// filename. Used by native drag-drop and the NFD picker.
    void addPath(const std::filesystem::path &path) override;

    /// Add a file by name + bytes: store directly in the bag's
    /// in-memory map. Used by web drag-drop's async fetch callback and
    /// the JS file-picker C export.
    void addBytes(std::string_view filename, std::span<const std::byte> bytes) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
