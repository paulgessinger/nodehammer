#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"
#include "scene_build_job_web_backend.hpp"

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

// ── CooperativeBackend ────────────────────────────────────────────────────────
//
// The original single-threaded web path: runs the (synchronous) prep on the
// first poll, then drives the WedgeCutJob / TessellationJob iterators across
// subsequent polls within `budget_ns` so the UI keeps rendering. This is the
// fallback whenever a Web Worker isn't usable (file://, no `Worker`, CSP,
// `?compute=main`).
namespace {

class CooperativeBackend final : public IWebBackend {
  public:
    void start(std::shared_ptr<const ::nodehammer::NHConfig> config,
               std::shared_ptr<const ::nodehammer::SemanticScene> scene, std::string config_label,
               std::string geometry_label,
               std::optional<::nodehammer::WedgeCutParams> wedge_cut) override {
        config_label_ = std::move(config_label);
        geometry_label_ = std::move(geometry_label);
        preset_config_ = std::move(config);
        preset_scene_ = std::move(scene);
        wedge_cut_ = wedge_cut;
        result_ = {};
        prep_ = {};
        wedge_job_ = ::nodehammer::WedgeCutJob{};
        tess_job_ = ::nodehammer::TessellationJob{};
        state_ = State::Queued;
    }

    bool poll(std::uint64_t budget_ns) override {
        switch (state_) {
        case State::Idle:
            return false;
        case State::Done:
            return true;
        case State::Queued:
            // Burn one poll so the caller's last frame paints before we run the
            // synchronous prep + first tessellation slice.
            state_ = State::PrepPending;
            return false;
        case State::PrepPending: {
            logPreBuild(config_label_, geometry_label_);
            prep_ = ::nodehammer::prepareSceneForTessellationFromInputs(
                *preset_config_, *preset_scene_, std::nullopt);
            preset_config_.reset();
            preset_scene_.reset();
            if (!prep_.ok) {
                result_.scene = nullptr;
                result_.diags = std::move(prep_.diags);
                state_ = State::Done;
                return true;
            }
            if (wedge_cut_) {
                wedge_job_.start(prep_.scene, *wedge_cut_);
                state_ = State::Cutting;
                if (wedge_job_.advance(budget_ns)) {
                    (void)wedge_job_.take();
                    tess_job_.start(prep_.config, prep_.scene);
                    state_ = State::Tessellating;
                }
                return false;
            }
            tess_job_.start(prep_.config, prep_.scene);
            state_ = State::Tessellating;
            if (tess_job_.advance(budget_ns)) {
                state_ = State::Finalizing;
            }
            return false;
        }
        case State::Cutting:
            if (wedge_job_.advance(budget_ns)) {
                (void)wedge_job_.take();
                tess_job_.start(prep_.config, prep_.scene);
                state_ = State::Tessellating;
            }
            return false;
        case State::Tessellating:
            if (tess_job_.advance(budget_ns)) {
                state_ = State::Finalizing;
            }
            return false;
        case State::Finalizing: {
            ::nodehammer::TessellationPassResult tess = tess_job_.take();
            prep_.diags.append(tess.diags);
            if (tess.diags.hasErrors()) {
                result_.scene = nullptr;
            } else {
                result_.scene = std::make_shared<::nodehammer::RenderScene>(std::move(tess.scene));
            }
            result_.diags = std::move(prep_.diags);
            prep_ = {};
            state_ = State::Done;
            return true;
        }
        }
        return false;
    }

    ::nodehammer::SceneBuildResult take() override {
        ::nodehammer::SceneBuildResult out = std::move(result_);
        result_ = {};
        state_ = State::Idle;
        config_label_.clear();
        geometry_label_.clear();
        prep_ = {};
        wedge_job_ = ::nodehammer::WedgeCutJob{};
        tess_job_ = ::nodehammer::TessellationJob{};
        return out;
    }

    std::size_t tessellationTotal() const override { return tess_job_.totalNodes(); }
    std::size_t tessellationProcessed() const override { return tess_job_.processedNodes(); }
    std::size_t wedgeCutTotal() const override { return wedge_job_.totalPlacements(); }
    std::size_t wedgeCutProcessed() const override { return wedge_job_.processedPlacements(); }

    SceneBuildJob::Phase phase() const override {
        switch (state_) {
        case State::Idle:
            return SceneBuildJob::Phase::Idle;
        case State::Queued:
        case State::PrepPending:
            return SceneBuildJob::Phase::Preparing;
        case State::Cutting:
            return SceneBuildJob::Phase::Cutting;
        case State::Tessellating:
            return SceneBuildJob::Phase::Tessellating;
        case State::Finalizing:
            return SceneBuildJob::Phase::Finalizing;
        case State::Done:
            return SceneBuildJob::Phase::Done;
        }
        return SceneBuildJob::Phase::Idle;
    }

  private:
    enum class State : std::uint8_t {
        Idle,
        Queued,       // start() called; first poll paints a "Tessellating…" frame
        PrepPending,  // run upstream stages on next poll
        Cutting,      // drive the cooperative WedgeCutJob (only when requested)
        Tessellating, // drive TessellationJob iterator
        Finalizing,   // package result on next poll
        Done,
    };
    State state_{State::Idle};

    ::nodehammer::ScenePrepResult prep_;
    ::nodehammer::WedgeCutJob wedge_job_;
    ::nodehammer::TessellationJob tess_job_;

    std::string config_label_;
    std::string geometry_label_;

    std::shared_ptr<const ::nodehammer::NHConfig> preset_config_;
    std::shared_ptr<const ::nodehammer::SemanticScene> preset_scene_;
    std::optional<::nodehammer::WedgeCutParams> wedge_cut_;

    ::nodehammer::SceneBuildResult result_;
};

} // namespace

std::unique_ptr<IWebBackend> makeCooperativeBackend() {
    return std::make_unique<CooperativeBackend>();
}

// ── SceneBuildJob: backend selection + delegation ─────────────────────────────
//
// Prefer the Web Worker (true off-main-thread parallelism); fall back to the
// cooperative state machine when one isn't available. The choice is made once,
// at construction, and is invisible to the App — both backends present the
// identical SceneBuildJob surface.
struct SceneBuildJob::Impl {
    std::unique_ptr<IWebBackend> backend;

    Impl() {
        backend = makeWorkerBackend();
        if (!backend) {
            backend = makeCooperativeBackend();
        }
    }
};

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}
SceneBuildJob::~SceneBuildJob() = default;

void SceneBuildJob::start(std::shared_ptr<const ::nodehammer::NHConfig> config,
                          std::shared_ptr<const ::nodehammer::SemanticScene> scene,
                          std::string config_label, std::string geometry_label,
                          std::optional<::nodehammer::WedgeCutParams> wedge_cut) {
    impl_->backend->start(std::move(config), std::move(scene), std::move(config_label),
                          std::move(geometry_label), wedge_cut);
}

bool SceneBuildJob::poll(uint64_t budget_ns) { return impl_->backend->poll(budget_ns); }

::nodehammer::SceneBuildResult SceneBuildJob::take() { return impl_->backend->take(); }

size_t SceneBuildJob::tessellationTotal() const { return impl_->backend->tessellationTotal(); }
size_t SceneBuildJob::tessellationProcessed() const {
    return impl_->backend->tessellationProcessed();
}
size_t SceneBuildJob::wedgeCutTotal() const { return impl_->backend->wedgeCutTotal(); }
size_t SceneBuildJob::wedgeCutProcessed() const { return impl_->backend->wedgeCutProcessed(); }

SceneBuildJob::Phase SceneBuildJob::phase() const { return impl_->backend->phase(); }

} // namespace nodehammer::viewer
