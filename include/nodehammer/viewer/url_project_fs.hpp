#pragma once

#include <nodehammer/viewer/project_fs.hpp>

#include <memory>
#include <string>

namespace nodehammer::viewer {

/// ProjectFs backend that downloads a TOML config, its (transitive)
/// `include = …` dependencies, and a geometry blob via emscripten_fetch
/// (web) and writes each into MEMFS at its URL-derived path. Includes are
/// discovered iteratively as each TOML file lands.
///
/// Web-only: this header is included from viewer_main.cpp which is itself
/// part of the wasm target. The native build uses the synchronous
/// buildSceneFromPaths flow in cmd_viewer.cpp instead and never instantiates
/// this class.
class UrlProjectFs final : public ProjectFs {
  public:
    UrlProjectFs();
    ~UrlProjectFs() override;

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
    ProjectFsStatus status() const override;
    std::span<const ProjectProgress> progress() const override;
    const std::string &errorMessage() const override;
    std::string_view name() const override { return "url"; }
    const std::filesystem::path &rootConfigPath() const override;
    const std::filesystem::path &rootInputPath() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
