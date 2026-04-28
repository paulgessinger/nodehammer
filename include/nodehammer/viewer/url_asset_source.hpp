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

    /// Begin fetching `config_url` and `input_url`. Both are MEMFS-style
    /// paths (typically root-relative, e.g. `/scene.toml`) and double as
    /// the on-disk locations the asset is written to.
    ///
    /// `asset_base` is an optional URL prefix prepended to the MEMFS path
    /// at fetch time only (so the file lands at `/scene.toml` in MEMFS but
    /// is downloaded from `<asset_base>/scene.toml`). Empty for root-served
    /// deployments; set to the document directory (e.g. `/foo/bar`) for
    /// sub-path deployments. Must not end in a trailing slash.
    void start(std::string config_url, std::string input_url, std::string asset_base = {});

    void poll() override;
    LoadState state() const override;
    std::span<const AssetProgress> progress() const override;
    const std::string &errorMessage() const override;
    const std::filesystem::path &configPath() const override;
    const std::filesystem::path &inputPath() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
