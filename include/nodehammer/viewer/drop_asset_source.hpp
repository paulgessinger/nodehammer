#pragma once

#include <nodehammer/viewer/asset_source.hpp>

#include <memory>

namespace nodehammer::viewer {

/// AssetSource representing a single drag-and-drop or file-picker gesture.
/// One instance is constructed per gesture by the platform layer, populated
/// with each file the gesture covers (paths on native, bytes on web), and
/// then installed on the App. The source does not accumulate across
/// gestures: dropping again creates a fresh instance and replaces the
/// previous one. Becomes `Ready` once both a config (.toml) and a
/// recognised geometry input have been added; reports a per-slot "still
/// missing" hint in the meantime so the UI can prompt the user to drop the
/// missing file alongside the rest next time.
class DropAssetSource final : public AssetSource {
  public:
    DropAssetSource();
    ~DropAssetSource() override;

    void poll() override;
    LoadState state() const override;
    std::span<const AssetProgress> progress() const override;
    const std::string &errorMessage() const override;
    const std::filesystem::path &configPath() const override;
    const std::filesystem::path &inputPath() const override;
    std::span<const std::string> waitingFor() const override;
    const std::string &unrecognised() const override;

    /// Add a file by path. Recognises .toml as the config slot and asks the
    /// importer registry for the input slot; unrecognised filenames are
    /// surfaced via `unrecognised()`. A second add for the same role
    /// overwrites the slot.
    void addPath(const std::filesystem::path &path);

    /// Add a file by name + bytes. Stages the bytes under the OS temp dir
    /// (which is `/tmp` under Emscripten) and then runs the same role
    /// recognition as `addPath`. Used by the web drop fetch callback and
    /// the JS file-picker C export.
    void addBytes(std::string_view filename, std::span<const std::byte> bytes);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
