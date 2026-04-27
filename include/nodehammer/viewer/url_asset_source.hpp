#pragma once

#include <nodehammer/viewer/asset_source.hpp>

#include <memory>

namespace nodehammer::viewer {

/// AssetSource that downloads a TOML config, its (transitive) `include = …`
/// dependencies, and a geometry blob via emscripten_fetch (web) and writes
/// each into MEMFS at its URL-derived path. Includes are discovered
/// iteratively as each TOML file lands.
///
/// On native this is a stub: start() resolves immediately to Ready (the
/// files already exist on disk and the synchronous CLI path is taken
/// instead). The class still exists in both builds so the App's polling
/// code compiles without ifdefs at the call site.
class UrlAssetSource final : public AssetSource {
  public:
    UrlAssetSource();
    ~UrlAssetSource() override;

    /// Begin fetching `config_url` and `input_url`. Both must be paths the
    /// browser can resolve (typically root-relative, e.g. `/scene.toml`).
    void start(std::string config_url, std::string input_url);

    void poll() override;
    LoadState state() const override;
    std::span<const AssetProgress> progress() const override;
    const std::string &error_message() const override;
    const std::filesystem::path &config_path() const override;
    const std::filesystem::path &input_path() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
