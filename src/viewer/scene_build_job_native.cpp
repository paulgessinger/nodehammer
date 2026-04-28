#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/tessellation_job.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace nodehammer::viewer {

struct SceneBuildJob::Impl {
    enum class State : uint8_t { Idle, Running, Done };
    State state{State::Idle};
    std::thread worker;
    std::atomic<bool> done{false};

    // Owned by the worker thread while running; main thread reads only the
    // atomics until the worker has signalled `done`.
    ::nodehammer::ScenePrepResult prep;
    ::nodehammer::TessellationJob tess_job;

    std::filesystem::path config_path;
    std::filesystem::path input_path;
    ::nodehammer::SceneBuildResult result;
};

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}

SceneBuildJob::~SceneBuildJob() {
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

void SceneBuildJob::start(std::filesystem::path config_path, std::filesystem::path input_path) {
    impl_->config_path = std::move(config_path);
    impl_->input_path = std::move(input_path);
    impl_->result = {};

    impl_->prep = {};
    impl_->tess_job = ::nodehammer::TessellationJob{};

    impl_->done.store(false, std::memory_order_release);
    impl_->state = Impl::State::Running;
    logPreBuild(impl_->config_path, impl_->input_path);
    impl_->worker = std::thread([impl = impl_.get()] {
        impl->prep = ::nodehammer::prepareSceneForTessellation(impl->config_path, impl->input_path);
        if (!impl->prep.ok) {
            impl->result.scene = nullptr;
            impl->result.diags = std::move(impl->prep.diags);
            impl->done.store(true, std::memory_order_release);
            return;
        }
        impl->tess_job.start(impl->prep.config, impl->prep.scene);
        // Run the iterator straight through — the thread isn't budget-
        // driven, but `totalNodes` / `processedNodes` get updated as we
        // go, so the main-thread UI can show a real-time bar.
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
    impl_->config_path.clear();
    impl_->input_path.clear();
    impl_->prep = {};
    impl_->tess_job = ::nodehammer::TessellationJob{};
    return out;
}

size_t SceneBuildJob::tessellationTotal() const { return impl_->tess_job.totalNodes(); }
size_t SceneBuildJob::tessellationProcessed() const { return impl_->tess_job.processedNodes(); }

SceneBuildJob::Phase SceneBuildJob::phase() const {
    switch (impl_->state) {
    case Impl::State::Idle:
        return Phase::Idle;
    case Impl::State::Running:
        // Native runs the whole pipeline on a worker thread — collapse
        // it under "Tessellating" since that's by far the longest stage
        // and the UI doesn't have visibility into the thread's progress.
        return Phase::Tessellating;
    case Impl::State::Done:
        return Phase::Done;
    }
    return Phase::Idle;
}

} // namespace nodehammer::viewer
