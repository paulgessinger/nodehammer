#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace nodehammer::viewer {

enum class LoadState { Idle, Fetching, Ready, Error };

/// Per-file download progress as observed by the loader. bytes_total is 0
/// until the server's Content-Length arrives (or stays 0 if the response is
/// chunked without one). The UI should fall back to an indeterminate display
/// in that case.
struct AssetProgress {
    std::string url;
    std::uint64_t bytes_done{0};
    std::uint64_t bytes_total{0};
    bool done{false};
    bool failed{false};
};

/// Asynchronously downloads a TOML config, its (transitive) `include = [...]`
/// dependencies, and a geometry blob, and writes each into MEMFS at its
/// URL-derived path. The viewer App polls state() each frame and, once it
/// reaches Ready, runs build_scene_from_paths against the materialised paths.
///
/// On native this is a stub: start() resolves immediately to Ready (the files
/// already exist on disk and the synchronous CLI path is taken instead). The
/// class still exists so the App's web-only progress UI compiles in both
/// builds without ifdefs at the call site.
class AssetLoader {
  public:
    AssetLoader();
    ~AssetLoader();
    AssetLoader(const AssetLoader &) = delete;
    AssetLoader &operator=(const AssetLoader &) = delete;

    /// Begin fetching `config_url` and `input_url`. Both must be paths that
    /// the browser can resolve (typically root-relative, e.g. `/scene.toml`).
    /// Includes are discovered automatically as each TOML file lands.
    void start(std::string config_url, std::string input_url);

    /// No-op on web (callbacks drive state transitions); native stub may use
    /// it later. Cheap to call every frame.
    void poll();

    LoadState state() const;

    /// Live view of every URL the loader is tracking. The vector grows as
    /// new includes are discovered; entries are never removed.
    std::span<const AssetProgress> progress() const;

    /// Set when state() == Error. Empty otherwise.
    const std::string &error_message() const;

    /// Valid once state() == Ready. The MEMFS path the config and geometry
    /// were written to (same shape as the URL passed to start()).
    const std::string &config_path() const;
    const std::string &input_path() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
