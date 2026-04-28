#pragma once

#include <nodehammer/viewer/project_fs.hpp>

#include <memory>

namespace nodehammer::viewer {

/// ProjectFs backend that accumulates files from drag-drop and file-picker
/// gestures. Long-lived (App-owned, not per-gesture), so dropping a config
/// in one gesture and a geometry in the next adds to the same bag rather
/// than replacing it. Becomes Ready once both a `.toml` and a recognised
/// geometry input have landed; reports per-slot "still missing" hints in
/// the meantime so the UI can prompt the user.
///
/// Last-write wins on basename collisions: if the user drops a second
/// `foo.toml`, the new path replaces the old, and a "replaced foo.toml"
/// note shows up in `warnings()`.
class BagProjectFs final : public ProjectFs {
  public:
    BagProjectFs();
    ~BagProjectFs() override;

    void poll() override;
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    const std::string &errorMessage() const override;
    const std::filesystem::path &rootConfigPath() const override;
    const std::filesystem::path &rootInputPath() const override;
    std::span<const std::string> waitingFor() const override;
    std::span<const std::string> warnings() const override;
    const std::string &unrecognised() const override;

    /// Add a file by path. Recognises `.toml` (case-insensitive) as the
    /// config slot and asks the importer registry for the input slot;
    /// unrecognised filenames are surfaced via `unrecognised()`.
    void addPath(const std::filesystem::path &path) override;

    /// Add a file by name + bytes. Stages the bytes under the OS temp dir
    /// (which resolves to `/tmp` under Emscripten) and then runs the same
    /// role recognition as `addPath`. Used by web drop fetch callbacks
    /// and the JS file-picker C export.
    void addBytes(std::string_view filename, std::span<const std::byte> bytes) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
