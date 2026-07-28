#include <nodehammer/tessellation/build_pipeline.hpp>

#include <nodehammer/tessellation/tessellation_job.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace nodehammer {

// The pipeline body is the canonical lift of the web-cooperative state machine
// (formerly CooperativeBackend::poll): a Queued → Preparing → (Cutting?) →
// Tessellating → Finalizing → Done walk, where the first advance() burns a
// frame (Queued → Preparing) and the actual synchronous prep runs on entry to
// the Preparing state. Every viewer backend and the synchronous shim now drive
// this one machine.
struct BuildPipeline::Impl {
    Phase phase{Phase::Idle};

    // Inputs held as shared_ptr<const>; prep takes its deep copy lazily on the
    // first Preparing advance (never in start()), so the caller's pristine
    // scene is never mutated (invariant #5) and the copy runs off the caller's
    // hot path (native: on the worker thread; cooperative: after a paint).
    std::shared_ptr<const NHConfig> preset_config;
    std::shared_ptr<const detail::SemanticScene> preset_scene;
    std::optional<WedgeCutParams> wedge_cut;

    ScenePrepResult prep;
    WedgeCutJob wedge_job;
    TessellationJob tess_job;

    SceneBuildResult result;

    void reset() {
        phase = Phase::Idle;
        preset_config.reset();
        preset_scene.reset();
        wedge_cut.reset();
        prep = {};
        wedge_job = WedgeCutJob{};
        tess_job = TessellationJob{};
        result = {};
    }
};

BuildPipeline::BuildPipeline() : impl_(std::make_unique<Impl>()) {}
BuildPipeline::~BuildPipeline() = default;
BuildPipeline::BuildPipeline(BuildPipeline &&) noexcept = default;
BuildPipeline &BuildPipeline::operator=(BuildPipeline &&) noexcept = default;

void BuildPipeline::start(std::shared_ptr<const NHConfig> config,
                          std::shared_ptr<const detail::SemanticScene> scene,
                          std::optional<WedgeCutParams> wedgeCut) {
    impl_->preset_config = std::move(config);
    impl_->preset_scene = std::move(scene);
    impl_->wedge_cut = wedgeCut;
    impl_->result = {};
    impl_->prep = {};
    impl_->wedge_job = WedgeCutJob{};
    impl_->tess_job = TessellationJob{};
    impl_->phase = Phase::Queued;
}

bool BuildPipeline::advance(std::uint64_t budget_ns) {
    Impl &s = *impl_;
    switch (s.phase) {
    case Phase::Idle:
        return false;
    case Phase::Done:
        return true;
    case Phase::Queued:
        // Burn one advance so a frame-driven caller's last frame paints before
        // we run the synchronous prep + first work slice.
        s.phase = Phase::Preparing;
        return false;
    case Phase::Preparing: {
        // Deferred-wedge prep (invariant #1): the wedge is always run as a
        // separate WedgeCutJob below, never inline in prep. Prep copies its
        // inputs by value, so the pristine scene stays untouched (invariant #5).
        s.prep =
            prepareSceneForTessellationFromInputs(*s.preset_config, *s.preset_scene, std::nullopt);
        s.preset_config.reset();
        s.preset_scene.reset();
        if (!s.prep.ok) {
            s.result.scene = nullptr;
            s.result.diags = std::move(s.prep.diags);
            s.phase = Phase::Done;
            return true;
        }
        if (s.wedge_cut) {
            s.wedge_job.start(s.prep.scene, *s.wedge_cut);
            s.phase = Phase::Cutting;
            if (s.wedge_job.advance(budget_ns)) {
                (void)s.wedge_job.take();
                s.tess_job.start(s.prep.config, s.prep.scene);
                s.phase = Phase::Tessellating;
            }
            return false;
        }
        s.tess_job.start(s.prep.config, s.prep.scene);
        s.phase = Phase::Tessellating;
        if (s.tess_job.advance(budget_ns)) {
            s.phase = Phase::Finalizing;
        }
        return false;
    }
    case Phase::Cutting:
        if (s.wedge_job.advance(budget_ns)) {
            (void)s.wedge_job.take();
            s.tess_job.start(s.prep.config, s.prep.scene);
            s.phase = Phase::Tessellating;
        }
        return false;
    case Phase::Tessellating:
        if (s.tess_job.advance(budget_ns)) {
            s.phase = Phase::Finalizing;
        }
        return false;
    case Phase::Finalizing: {
        // Result-packaging tail (invariant #4): append tessellation diags to
        // prep's, gate the scene on hasErrors(), and hand back a shared scene.
        TessellationPassResult tess = s.tess_job.take();
        s.prep.diags.append(tess.diags);
        if (tess.diags.hasErrors()) {
            s.result.scene = nullptr;
        } else {
            s.result.scene = std::make_shared<detail::RenderScene>(std::move(tess.scene));
        }
        s.result.diags = std::move(s.prep.diags);
        s.prep = {};
        s.phase = Phase::Done;
        return true;
    }
    }
    return false;
}

SceneBuildResult BuildPipeline::take() {
    SceneBuildResult out = std::move(impl_->result);
    impl_->reset();
    return out;
}

BuildPipeline::Phase BuildPipeline::phase() const { return impl_->phase; }

std::size_t BuildPipeline::tessellationTotal() const { return impl_->tess_job.totalNodes(); }
std::size_t BuildPipeline::tessellationProcessed() const {
    return impl_->tess_job.processedNodes();
}
std::size_t BuildPipeline::wedgeCutTotal() const { return impl_->wedge_job.totalPlacements(); }
std::size_t BuildPipeline::wedgeCutProcessed() const {
    return impl_->wedge_job.processedPlacements();
}

} // namespace nodehammer
