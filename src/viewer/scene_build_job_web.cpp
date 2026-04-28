#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/tessellation_job.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace nodehammer::viewer {

struct SceneBuildJob::Impl {
    enum class State : uint8_t {
        Idle,
        Queued,       // start() called, first poll will paint a "Tessellating…" frame
        PrepPending,  // run upstream stages (config, import, select, dedup) on next poll
        Tessellating, // drive TessellationJob iterator
        Finalizing,   // package result on next poll
        Done,
    };
    State state{State::Idle};

    ::nodehammer::ScenePrepResult prep;
    ::nodehammer::TessellationJob tess_job;

    std::string config_path;
    std::string input_path;
    ::nodehammer::SceneBuildResult result;
};

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}
SceneBuildJob::~SceneBuildJob() = default;

void SceneBuildJob::start(std::string config_path, std::string input_path) {
    impl_->config_path = std::move(config_path);
    impl_->input_path = std::move(input_path);
    impl_->result = {};
    impl_->prep = {};
    impl_->tess_job = ::nodehammer::TessellationJob{};

    // Web: defer all real work by one poll so the caller can paint a
    // "Tessellating…" frame before the upstream stages run.
    impl_->state = Impl::State::Queued;
}

bool SceneBuildJob::poll(uint64_t budget_ns) {
    switch (impl_->state) {
    case Impl::State::Idle:
        return false;
    case Impl::State::Done:
        return true;
    case Impl::State::Queued:
        // Burn one poll so the caller's last frame paints before we run
        // the synchronous prep + first tessellation slice.
        impl_->state = Impl::State::PrepPending;
        return false;
    case Impl::State::PrepPending: {
        logPreBuild(impl_->config_path, impl_->input_path);
        impl_->prep =
            ::nodehammer::prepareSceneForTessellation(impl_->config_path, impl_->input_path);
        if (!impl_->prep.ok) {
            // Upstream stage failed — package the diags and finish.
            impl_->result.scene = nullptr;
            impl_->result.diags = std::move(impl_->prep.diags);
            impl_->state = Impl::State::Done;
            return true;
        }
        impl_->tess_job.start(impl_->prep.config, impl_->prep.scene);
        impl_->state = Impl::State::Tessellating;
        // Run a first slice immediately so we make visible progress on
        // this poll — but stop within the budget so we yield to render.
        if (impl_->tess_job.advance(budget_ns)) {
            impl_->state = Impl::State::Finalizing;
        }
        return false;
    }
    case Impl::State::Tessellating:
        if (impl_->tess_job.advance(budget_ns)) {
            impl_->state = Impl::State::Finalizing;
        }
        return false;
    case Impl::State::Finalizing: {
        ::nodehammer::TessellationPassResult tess = impl_->tess_job.take();
        impl_->prep.diags.append(tess.diags);
        if (tess.diags.hasErrors()) {
            impl_->result.scene = nullptr;
        } else {
            impl_->result.scene =
                std::make_shared<::nodehammer::RenderScene>(std::move(tess.scene));
        }
        impl_->result.diags = std::move(impl_->prep.diags);
        impl_->prep = {};
        impl_->state = Impl::State::Done;
        return true;
    }
    }
    return false;
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
    case Impl::State::Queued:
    case Impl::State::PrepPending:
        return Phase::Preparing;
    case Impl::State::Tessellating:
        return Phase::Tessellating;
    case Impl::State::Finalizing:
        return Phase::Finalizing;
    case Impl::State::Done:
        return Phase::Done;
    }
    return Phase::Idle;
}

} // namespace nodehammer::viewer
