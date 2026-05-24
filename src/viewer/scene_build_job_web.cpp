#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/tessellation_job.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace nodehammer::viewer {

struct SceneBuildJob::Impl {
    enum class State : uint8_t {
        Idle,
        Queued,       // start() called, first poll will paint a "Tessellating…" frame
        PrepPending,  // run upstream stages (config, import, select, dedup) on next poll
        Cutting,      // drive the cooperative WedgeCutJob (only when one is requested)
        Tessellating, // drive TessellationJob iterator
        Finalizing,   // package result on next poll
        Done,
    };
    State state{State::Idle};

    ::nodehammer::ScenePrepResult prep;
    ::nodehammer::WedgeCutJob wedge_job;
    ::nodehammer::TessellationJob tess_job;

    std::string config_label;
    std::string geometry_label;

    // Pre-prepared inputs handed in by `start`. shared_ptr<const> to match the
    // native signature; on web (single-threaded) the copy prep consumes is
    // unavoidably on the main thread, but it lands on the PrepPending poll —
    // after a frame has painted "Tessellating…" — rather than inside start().
    std::shared_ptr<const ::nodehammer::NHConfig> preset_config;
    std::shared_ptr<const ::nodehammer::SemanticScene> preset_scene;
    std::optional<::nodehammer::WedgeCutParams> wedge_cut;

    ::nodehammer::SceneBuildResult result;
};

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}
SceneBuildJob::~SceneBuildJob() = default;

void SceneBuildJob::start(std::shared_ptr<const ::nodehammer::NHConfig> config,
                          std::shared_ptr<const ::nodehammer::SemanticScene> scene,
                          std::string config_label, std::string geometry_label,
                          std::optional<::nodehammer::WedgeCutParams> wedge_cut) {
    impl_->config_label = std::move(config_label);
    impl_->geometry_label = std::move(geometry_label);
    impl_->preset_config = std::move(config);
    impl_->preset_scene = std::move(scene);
    impl_->wedge_cut = wedge_cut;
    impl_->result = {};
    impl_->prep = {};
    impl_->wedge_job = ::nodehammer::WedgeCutJob{};
    impl_->tess_job = ::nodehammer::TessellationJob{};
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
        logPreBuild(impl_->config_label, impl_->geometry_label);
        // Prep (validate / select / dedup) runs without the wedge cut — the cut
        // is driven cooperatively below so it doesn't freeze the frame and can
        // report progress. Tests / CLI still get the synchronous cut via prep.
        impl_->prep = ::nodehammer::prepareSceneForTessellationFromInputs(
            *impl_->preset_config, *impl_->preset_scene, std::nullopt);
        impl_->preset_config.reset();
        impl_->preset_scene.reset();
        if (!impl_->prep.ok) {
            // Upstream stage failed — package the diags and finish.
            impl_->result.scene = nullptr;
            impl_->result.diags = std::move(impl_->prep.diags);
            impl_->state = Impl::State::Done;
            return true;
        }
        if (impl_->wedge_cut) {
            impl_->wedge_job.start(impl_->prep.scene, *impl_->wedge_cut);
            impl_->state = Impl::State::Cutting;
            // Run a first slice immediately so we make visible progress on
            // this poll — but stop within the budget so we yield to render.
            if (impl_->wedge_job.advance(budget_ns)) {
                (void)impl_->wedge_job.take();
                impl_->tess_job.start(impl_->prep.config, impl_->prep.scene);
                impl_->state = Impl::State::Tessellating;
            }
            return false;
        }
        impl_->tess_job.start(impl_->prep.config, impl_->prep.scene);
        impl_->state = Impl::State::Tessellating;
        if (impl_->tess_job.advance(budget_ns)) {
            impl_->state = Impl::State::Finalizing;
        }
        return false;
    }
    case Impl::State::Cutting:
        if (impl_->wedge_job.advance(budget_ns)) {
            (void)impl_->wedge_job.take();
            impl_->tess_job.start(impl_->prep.config, impl_->prep.scene);
            impl_->state = Impl::State::Tessellating;
        }
        return false;
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
    case Impl::State::Queued:
    case Impl::State::PrepPending:
        return Phase::Preparing;
    case Impl::State::Cutting:
        return Phase::Cutting;
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
