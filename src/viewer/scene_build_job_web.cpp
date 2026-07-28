#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"
#include "scene_build_job_web_backend.hpp"

#include <nodehammer/scene_build.hpp>
#include <nodehammer/tessellation/build_pipeline.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <utility>

namespace nodehammer::viewer {

// ── CooperativeBackend ────────────────────────────────────────────────────────
//
// The original single-threaded web path: runs the (synchronous) prep on the
// first poll, then drives the wedge/tessellation across subsequent polls within
// `budget_ns` so the UI keeps rendering. This is the fallback whenever a Web
// Worker isn't usable (file://, no `Worker`, CSP, `?compute=main`).
//
// Since Stage 7 it is a thin adapter over the shared core `BuildPipeline`: the
// frame-sliced state machine that used to live here now lives in the core lib
// (unit-tested there), so this backend just forwards `poll → advance` and
// delegates every getter. The strategy seam (IWebBackend) and the transparent
// worker→cooperative replay in SceneBuildJob::poll are unchanged — this is the
// only fallback that works under file:// / no-Worker / CSP / ?compute=main.
namespace {

class CooperativeBackend final : public IWebBackend {
  public:
    void start(std::shared_ptr<const ::nodehammer::NHConfig> config,
               std::shared_ptr<const ::nodehammer::detail::SemanticScene> scene,
               std::string config_label, std::string geometry_label,
               std::optional<::nodehammer::WedgeCutParams> wedge_cut) override {
        logPreBuild(config_label, geometry_label);
        pipe_.start(std::move(config), std::move(scene), wedge_cut);
    }

    bool poll(std::uint64_t budget_ns) override { return pipe_.advance(budget_ns); }

    ::nodehammer::SceneBuildResult take() override { return pipe_.take(); }

    std::size_t tessellationTotal() const override { return pipe_.tessellationTotal(); }
    std::size_t tessellationProcessed() const override { return pipe_.tessellationProcessed(); }
    std::size_t wedgeCutTotal() const override { return pipe_.wedgeCutTotal(); }
    std::size_t wedgeCutProcessed() const override { return pipe_.wedgeCutProcessed(); }

    SceneBuildJob::Phase phase() const override { return pipe_.phase(); }

  private:
    ::nodehammer::BuildPipeline pipe_;
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
    bool using_worker{false};

    // Saved start() args, kept only while the worker backend is active, so a
    // fatal worker failure mid-build can be replayed on the cooperative backend
    // without the App ever knowing.
    std::shared_ptr<const ::nodehammer::NHConfig> saved_config;
    std::shared_ptr<const ::nodehammer::detail::SemanticScene> saved_scene;
    std::string saved_config_label;
    std::string saved_geometry_label;
    std::optional<::nodehammer::WedgeCutParams> saved_wedge;

    Impl() {
        backend = makeWorkerBackend();
        using_worker = backend != nullptr;
        if (!backend) {
            backend = makeCooperativeBackend();
        }
    }
};

SceneBuildJob::SceneBuildJob() : impl_(std::make_unique<Impl>()) {}
SceneBuildJob::~SceneBuildJob() = default;

void SceneBuildJob::start(std::shared_ptr<const ::nodehammer::NHConfig> config,
                          std::shared_ptr<const ::nodehammer::detail::SemanticScene> scene,
                          std::string config_label, std::string geometry_label,
                          std::optional<::nodehammer::WedgeCutParams> wedge_cut) {
    if (impl_->using_worker) {
        // Cheap (shared_ptr refcount + small string copies) — retained for a
        // possible cooperative replay if the worker turns out to be broken.
        impl_->saved_config = config;
        impl_->saved_scene = scene;
        impl_->saved_config_label = config_label;
        impl_->saved_geometry_label = geometry_label;
        impl_->saved_wedge = wedge_cut;
    }
    impl_->backend->start(std::move(config), std::move(scene), std::move(config_label),
                          std::move(geometry_label), wedge_cut);
}

bool SceneBuildJob::poll(uint64_t budget_ns) {
    const bool done = impl_->backend->poll(budget_ns);
    if (done && impl_->using_worker && impl_->backend->wantsFallback()) {
        // The worker is unusable (fatal load/run failure). Discard its empty
        // result and transparently rerun this build on the cooperative backend.
        (void)impl_->backend->take();
        std::println(std::cerr, "scene_build_job: compute worker unavailable -- falling back to "
                                "cooperative main-thread build");
        impl_->backend = makeCooperativeBackend();
        impl_->using_worker = false;
        impl_->backend->start(impl_->saved_config, impl_->saved_scene, impl_->saved_config_label,
                              impl_->saved_geometry_label, impl_->saved_wedge);
        impl_->saved_config.reset();
        impl_->saved_scene.reset();
        return false; // keep building, now cooperatively
    }
    return done;
}

::nodehammer::SceneBuildResult SceneBuildJob::take() { return impl_->backend->take(); }

size_t SceneBuildJob::tessellationTotal() const { return impl_->backend->tessellationTotal(); }
size_t SceneBuildJob::tessellationProcessed() const {
    return impl_->backend->tessellationProcessed();
}
size_t SceneBuildJob::wedgeCutTotal() const { return impl_->backend->wedgeCutTotal(); }
size_t SceneBuildJob::wedgeCutProcessed() const { return impl_->backend->wedgeCutProcessed(); }

SceneBuildJob::Phase SceneBuildJob::phase() const { return impl_->backend->phase(); }

} // namespace nodehammer::viewer
