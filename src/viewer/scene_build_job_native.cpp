#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"

#include <scene_build.hpp>
#include <tessellation/build_pipeline.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace nodehammer::viewer {

struct SceneBuildJob::Impl {
    enum class State : uint8_t { Idle, Running, Done };
    State state{State::Idle};
    std::thread worker;
    std::atomic<bool> done{false};
    // Finer-grained phase published by the worker so the UI can distinguish the
    // (cooperative) wedge cut from tessellation. Read by the main thread while
    // `state == Running`; relaxed ordering — the label tolerates staleness, and
    // BuildPipeline::phase() itself isn't safe to read concurrently, so we
    // mirror it into this atomic from the worker loop and the main thread reads
    // only the mirror.
    std::atomic<Phase> worker_phase{Phase::Preparing};

    // The shared build core. Owned and driven entirely by the worker thread
    // while running; the main thread only samples the progress counters (the
    // same tolerated cross-thread read the previous inline jobs had) and the
    // atomic phase mirror above.
    ::nodehammer::BuildPipeline pipe;

    // Diagnostic labels for the pre-build log. The byte-driven build job
    // doesn't have real filesystem paths — these are just whatever the
    // BuildSession resolved as the root keys (e.g. "scene.toml" or
    // "/path/to/scene.toml").
    std::string config_label;
    std::string geometry_label;

    // Pre-prepared inputs handed in by `start`. Held as shared_ptr<const> so
    // `start` (main thread) just refcounts; the worker hands them to the
    // pipeline, which takes the deep copy prep consumes on its first advance —
    // on this worker thread, off the main loop.
    std::shared_ptr<const ::nodehammer::NHConfig> preset_config;
    std::shared_ptr<const ::nodehammer::SemanticScene> preset_scene;
    std::optional<::nodehammer::WedgeCutParams> wedge_cut;
};

namespace {
// The build runs on a dedicated worker thread with no frame-responsiveness
// constraint, so the slice budget only governs how often the worker publishes
// its phase + progress counters to the main thread. A coarse slice keeps
// throughput near drive-to-completion while still updating the toast several
// times across a multi-second ODD build. (Driving with UINT64_MAX would run a
// whole phase inside one advance and leave the mirror/counters stale until it
// finished.)
constexpr std::uint64_t kNativeSliceNs = 50'000'000;
} // namespace

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}

SceneBuildJob::~SceneBuildJob() {
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void SceneBuildJob::start(std::shared_ptr<const ::nodehammer::NHConfig> config,
                          std::shared_ptr<const ::nodehammer::SemanticScene> scene,
                          std::string config_label, std::string geometry_label,
                          std::optional<::nodehammer::WedgeCutParams> wedge_cut) {
    impl_->config_label = std::move(config_label);
    impl_->geometry_label = std::move(geometry_label);
    impl_->preset_config = std::move(config);
    impl_->preset_scene = std::move(scene);
    impl_->wedge_cut = wedge_cut;

    impl_->done.store(false, std::memory_order_release);
    impl_->worker_phase.store(Phase::Preparing, std::memory_order_relaxed);
    impl_->state = Impl::State::Running;
    logPreBuild(impl_->config_label, impl_->geometry_label);
    impl_->worker = std::thread([impl = impl_.get()] {
        // The pipeline defers the wedge to a WedgeCutJob and takes its deep copy
        // of the inputs on the first advance — both happen here, on the worker
        // thread, so re-aiming the wedge cut never freezes the frame. The main
        // thread reads only `done` and the atomics until we signal completion.
        impl->pipe.start(std::move(impl->preset_config), std::move(impl->preset_scene),
                         impl->wedge_cut);
        while (!impl->pipe.advance(kNativeSliceNs)) {
            impl->worker_phase.store(impl->pipe.phase(), std::memory_order_relaxed);
        }
        impl->worker_phase.store(impl->pipe.phase(), std::memory_order_relaxed);
        // Leave the finished pipeline intact and only publish `done`. The main
        // thread drains the result via pipe.take() after it joins the worker
        // (see SceneBuildJob::take), so the progress getters that sample
        // tess_job/wedge_job can never race with take()'s reset of those jobs.
        impl->done.store(true, std::memory_order_release);
    });
}

bool SceneBuildJob::poll(uint64_t /*budget_ns*/) {
    if (impl_->state == Impl::State::Done) {
        return true;
    }
    if (impl_->state != Impl::State::Running) {
        return false;
    }
    if (!impl_->done.load(std::memory_order_acquire)) {
        return false;
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    impl_->state = Impl::State::Done;
    return true;
}

::nodehammer::SceneBuildResult SceneBuildJob::take() {
    // Only reached after poll() observed `done` and joined the worker, so
    // draining the pipeline here — which resets its nested wedge/tess jobs —
    // runs single-threaded and cannot race with the worker or the progress
    // getters that sample those jobs while the build is Running.
    ::nodehammer::SceneBuildResult out = impl_->pipe.take();
    impl_->state = Impl::State::Idle;
    impl_->config_label.clear();
    impl_->geometry_label.clear();
    return out;
}

size_t SceneBuildJob::tessellationTotal() const { return impl_->pipe.tessellationTotal(); }
size_t SceneBuildJob::tessellationProcessed() const { return impl_->pipe.tessellationProcessed(); }

size_t SceneBuildJob::wedgeCutTotal() const { return impl_->pipe.wedgeCutTotal(); }
size_t SceneBuildJob::wedgeCutProcessed() const { return impl_->pipe.wedgeCutProcessed(); }

SceneBuildJob::Phase SceneBuildJob::phase() const {
    switch (impl_->state) {
    case Impl::State::Idle:
        return Phase::Idle;
    case Impl::State::Running:
        // The whole pipeline runs on a worker thread; the worker publishes its
        // current stage (Preparing → Cutting → Tessellating) into an atomic so
        // the UI can label and bar each phase from the matching counters.
        return impl_->worker_phase.load(std::memory_order_relaxed);
    case Impl::State::Done:
        return Phase::Done;
    }
    return Phase::Idle;
}

} // namespace nodehammer::viewer
