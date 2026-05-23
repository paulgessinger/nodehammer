#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/tessellation_job.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
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
    // `state == Running`; relaxed ordering — the label tolerates staleness.
    std::atomic<Phase> worker_phase{Phase::Preparing};

    // Owned by the worker thread while running; main thread reads only the
    // atomics until the worker has signalled `done`.
    ::nodehammer::ScenePrepResult prep;
    ::nodehammer::WedgeCutJob wedge_job;
    ::nodehammer::TessellationJob tess_job;

    // Diagnostic labels for the pre-build log. The byte-driven build job
    // doesn't have real filesystem paths — these are just whatever the
    // BuildSession resolved as the root keys (e.g. "scene.toml" or
    // "/path/to/scene.toml").
    std::string config_label;
    std::string geometry_label;

    // Pre-prepared inputs handed in by `start`.
    std::unique_ptr<::nodehammer::NHConfig> preset_config;
    std::unique_ptr<::nodehammer::SemanticScene> preset_scene;
    std::optional<::nodehammer::WedgeCutParams> wedge_cut;

    ::nodehammer::SceneBuildResult result;
};

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}

SceneBuildJob::~SceneBuildJob() {
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void SceneBuildJob::start(::nodehammer::NHConfig config, ::nodehammer::SemanticScene scene,
                          std::string config_label, std::string geometry_label,
                          std::optional<::nodehammer::WedgeCutParams> wedge_cut) {
    impl_->config_label = std::move(config_label);
    impl_->geometry_label = std::move(geometry_label);
    impl_->preset_config = std::make_unique<::nodehammer::NHConfig>(std::move(config));
    impl_->preset_scene = std::make_unique<::nodehammer::SemanticScene>(std::move(scene));
    impl_->wedge_cut = wedge_cut;
    impl_->result = {};

    impl_->prep = {};
    impl_->wedge_job = ::nodehammer::WedgeCutJob{};
    impl_->tess_job = ::nodehammer::TessellationJob{};

    impl_->done.store(false, std::memory_order_release);
    impl_->worker_phase.store(Phase::Preparing, std::memory_order_relaxed);
    impl_->state = Impl::State::Running;
    logPreBuild(impl_->config_label, impl_->geometry_label);
    impl_->worker = std::thread([impl = impl_.get()] {
        // Prep (validate / select / dedup) runs without the wedge cut — the cut
        // is driven cooperatively below so we can publish its progress, matching
        // the web path. Tests / CLI still get the synchronous cut via prep.
        impl->prep = ::nodehammer::prepareSceneForTessellationFromInputs(
            std::move(*impl->preset_config), std::move(*impl->preset_scene), std::nullopt);
        impl->preset_config.reset();
        impl->preset_scene.reset();
        if (!impl->prep.ok) {
            impl->result.scene = nullptr;
            impl->result.diags = std::move(impl->prep.diags);
            impl->done.store(true, std::memory_order_release);
            return;
        }
        if (impl->wedge_cut) {
            impl->worker_phase.store(Phase::Cutting, std::memory_order_relaxed);
            impl->wedge_job.start(impl->prep.scene, *impl->wedge_cut);
            while (!impl->wedge_job.advance(std::numeric_limits<uint64_t>::max())) {
            }
            (void)impl->wedge_job.take();
        }
        impl->worker_phase.store(Phase::Tessellating, std::memory_order_relaxed);
        impl->tess_job.start(impl->prep.config, impl->prep.scene);
        while (!impl->tess_job.advance(std::numeric_limits<uint64_t>::max())) {
        }
        ::nodehammer::TessellationPassResult tess = impl->tess_job.take();
        impl->prep.diags.append(tess.diags);
        if (tess.diags.hasErrors()) {
            impl->result.scene = nullptr;
        } else {
            impl->result.scene = std::make_shared<::nodehammer::RenderScene>(std::move(tess.scene));
        }
        impl->result.diags = std::move(impl->prep.diags);
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
    ::nodehammer::SceneBuildResult out = std::move(impl_->result);
    impl_->result = {};
    impl_->state = Impl::State::Idle;
    impl_->config_label.clear();
    impl_->geometry_label.clear();
    impl_->prep = {};
    impl_->wedge_job = ::nodehammer::WedgeCutJob{};
    impl_->tess_job = ::nodehammer::TessellationJob{};
    return out;
}

size_t SceneBuildJob::tessellationTotal() const { return impl_->tess_job.totalNodes(); }
size_t SceneBuildJob::tessellationProcessed() const { return impl_->tess_job.processedNodes(); }

size_t SceneBuildJob::wedgeCutTotal() const { return impl_->wedge_job.totalPlacements(); }
size_t SceneBuildJob::wedgeCutProcessed() const { return impl_->wedge_job.processedPlacements(); }

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
