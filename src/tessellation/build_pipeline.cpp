#include <tessellation/build_pipeline.hpp>

#include <tessellation/tessellation_job.hpp>
#include <tessellation/tessellation_pass.hpp>
#include <tessellation/wedge_cut.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace nodehammer::tessellation {

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
    std::shared_ptr<const config::NHConfig> preset_config;
    std::shared_ptr<const ir::semantic::Scene> preset_scene;
    std::optional<WedgeCutParams> wedge_cut;

    pipeline::ScenePrepResult prep;
    WedgeCutJob wedge_job;
    TessellationJob tess_job;

    pipeline::SceneBuildResult result;

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

void BuildPipeline::start(std::shared_ptr<const config::NHConfig> config,
                          std::shared_ptr<const ir::semantic::Scene> scene,
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
        //
        // This is the boundary the doctrine's asynchronous carve-out describes:
        // prep throws, and there is no caller here to unwind to — `advance` is
        // driven from a frame loop that wants a state machine, not a stack. So
        // the failure becomes `SceneBuildResult::failure`, which is the same
        // channel by other means (docs/error-model.md).
        try {
            s.prep = pipeline::prepareSceneForTessellationFromInputs(*s.preset_config,
                                                                     *s.preset_scene, std::nullopt);
        } catch (const Error &e) {
            s.preset_config.reset();
            s.preset_scene.reset();
            s.result.scene = nullptr;
            s.result.diags = std::move(s.prep.diags);
            s.result.failure = e;
            s.phase = Phase::Done;
            return true;
        }
        s.preset_config.reset();
        s.preset_scene.reset();
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
        // prep's and hand back a shared scene.
        //
        // The scene comes back even when tessellation reported errors. Those are
        // partial results — a node the pass could not mesh — and whether a scene
        // with a hole in it is worth looking at is the viewer's judgement, not
        // this driver's. It was throwing the whole build away over one unknown
        // shape, and saying so only in a list nobody had to read.
        TessellationPassResult tess = s.tess_job.take();
        s.prep.diags.append(tess.diags);
        s.result.scene = std::make_shared<ir::render::Scene>(std::move(tess.scene));
        s.result.diags = std::move(s.prep.diags);
        s.prep = {};
        s.phase = Phase::Done;
        return true;
    }
    }
    return false;
}

pipeline::SceneBuildResult BuildPipeline::take() {
    pipeline::SceneBuildResult out = std::move(impl_->result);
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

} // namespace nodehammer::tessellation
