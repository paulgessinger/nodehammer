#pragma once

#include <nodehammer/viewer/asset_source.hpp>

#include <memory>

namespace nodehammer::viewer {

/// AssetSource that ingests files nominated by the user via drag-and-drop or
/// a native file picker. Stays Idle until at least one file is offered, then
/// transitions to Ready once both a config (.toml) and a recognised
/// geometry input have been collected. Order doesn't matter; the App can
/// drop or pick files one at a time and the source accumulates them.
class LocalFileAssetSource final : public AssetSource {
  public:
    LocalFileAssetSource();
    ~LocalFileAssetSource() override;

    /// True if the source is still waiting on a config file (.toml).
    bool needsConfig() const;
    /// True if the source is still waiting on a geometry input.
    bool needsInput() const;

    /// Last filename ignored for not matching a known role, if any. Cleared
    /// once a recognised file arrives. Used by the App to surface a "don't
    /// know what to do with foo.bar" hint in the placeholder UI.
    const std::string &lastUnrecognised() const;

    void poll() override;
    LoadState state() const override;
    std::span<const AssetProgress> progress() const override;
    const std::string &errorMessage() const override;
    const std::filesystem::path &configPath() const override;
    const std::filesystem::path &inputPath() const override;
    void ingestLocalFile(const std::filesystem::path &path) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
