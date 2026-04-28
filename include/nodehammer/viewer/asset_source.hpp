#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace nodehammer::viewer {

enum class LoadState { Idle, Fetching, Ready, Error };

/// Per-asset progress as observed by an AssetSource. bytes_total is 0 until
/// the response Content-Length lands (or stays 0 if the underlying source
/// can't supply one — e.g. local file ingest just before fstat). The UI
/// should fall back to an indeterminate display when total is 0.
struct AssetProgress {
    std::string url;
    std::uint64_t bytes_done{0};
    std::uint64_t bytes_total{0};
    bool done{false};
    bool failed{false};
};

/// Abstract bytes-into-MEMFS pipeline for the viewer. Implementations
/// nominate how files arrive (URL fetch, drag-and-drop, file picker, future
/// container unpack); the App only cares about the polled state machine and
/// the resolved config/input paths once Ready.
class AssetSource {
  public:
    virtual ~AssetSource() = default;

    AssetSource() = default;
    AssetSource(const AssetSource &) = delete;
    AssetSource &operator=(const AssetSource &) = delete;

    /// Cheap to call every frame. Sources driven by external callbacks
    /// (emscripten_fetch, sokol drop events) leave this as a no-op; sources
    /// that need to advance their own state (e.g. native file reader) do
    /// their work here.
    virtual void poll() {}

    virtual LoadState state() const = 0;
    virtual std::span<const AssetProgress> progress() const = 0;
    virtual const std::string &errorMessage() const = 0;

    /// Valid once state() == Ready. Filesystem paths the App should hand to
    /// buildSceneFromPaths. On web these live in MEMFS; on native they
    /// point into the real filesystem (or MEMFS-equivalent for ingested
    /// uploads). May be empty if the source has not yet identified a file
    /// of that role (e.g. accumulator sources waiting on more user input —
    /// in which case state() will not be Ready).
    virtual const std::filesystem::path &configPath() const = 0;
    virtual const std::filesystem::path &inputPath() const = 0;

    /// Optional hook: hand a freshly-acquired local file (from drag-and-drop
    /// or a file picker) to the source. Sources that don't accept user
    /// uploads ignore this. Used by the App to plumb sokol's FILES_DROPPED
    /// event and the NFD picker into the same code path.
    virtual void ingestLocalFile(const std::filesystem::path & /*path*/) {}
};

} // namespace nodehammer::viewer
