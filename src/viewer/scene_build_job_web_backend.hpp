#pragma once

#include "scene_build_job.hpp"

#include <scene_build.hpp>
#include <tessellation/wedge_cut.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace nodehammer::viewer {

// Web-only pluggable backend behind SceneBuildJob. Two implementations exist:
//
//   * CooperativeBackend — the single-threaded state machine that slices the
//     build across frames on the main thread (the original web path; the only
//     one that works under file:// and the lowest-memory option).
//   * WorkerBackend — ships the build to a Web Worker hosting a second wasm
//     instance (true off-main-thread parallelism, no SharedArrayBuffer).
//
// SceneBuildJob picks one at construction; the public interface and the App are
// identical either way. The interface mirrors SceneBuildJob's surface exactly.
class IWebBackend {
  public:
    virtual ~IWebBackend() = default;

    virtual void start(std::shared_ptr<const ::nodehammer::NHConfig> config,
                       std::shared_ptr<const ::nodehammer::SemanticScene> scene,
                       std::string config_label, std::string geometry_label,
                       std::optional<::nodehammer::WedgeCutParams> wedge_cut) = 0;
    virtual bool poll(std::uint64_t budget_ns) = 0;
    [[nodiscard]] virtual ::nodehammer::SceneBuildResult take() = 0;

    [[nodiscard]] virtual std::size_t tessellationTotal() const = 0;
    [[nodiscard]] virtual std::size_t tessellationProcessed() const = 0;
    [[nodiscard]] virtual std::size_t wedgeCutTotal() const = 0;
    [[nodiscard]] virtual std::size_t wedgeCutProcessed() const = 0;
    [[nodiscard]] virtual SceneBuildJob::Phase phase() const = 0;

    /// True once the backend is known to be unusable and the just-finished
    /// build should be retried on a different backend (the worker backend
    /// raises this on a fatal worker/module failure). Default: never.
    [[nodiscard]] virtual bool wantsFallback() const { return false; }
};

/// Always succeeds — the cooperative path has no external dependency.
std::unique_ptr<IWebBackend> makeCooperativeBackend();

/// Returns nullptr when Web Workers are unavailable or force-disabled
/// (e.g. file:// origin, no `Worker`, or `?compute=main`), so the caller
/// falls back to the cooperative backend.
std::unique_ptr<IWebBackend> makeWorkerBackend();

} // namespace nodehammer::viewer
