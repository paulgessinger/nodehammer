#pragma once

#include "scene_build_job.hpp"
#include "ui/notifications.hpp"

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/tessellation/wedge_cut.hpp>
#include <nodehammer/viewer/build_session.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace nodehammer::viewer {

/// Owns the viewer's CPU-side build orchestration — the `BuildSession` (walk →
/// resolve → parse → import), the off-loop `SceneBuildJob` (validate → select →
/// dedup → wedge → tessellate), the pristine-inputs cache, and the Boolean-cut
/// bookkeeping — all pulled out of `App::Impl`. It rides on the Stage-7
/// `SceneBuildJob`/`BuildPipeline` rework.
///
/// GPU work stays in the App: the controller only produces CPU `RenderScene`s
/// and reports them through callbacks (base scene, cut scene, and a
/// "build starting" hook so the App can drop the resident cut bake). The App
/// still owns `scene`/`cut_scene`, the GPU uploads, and the camera framing.
class BuildController {
  public:
    /// The live azimuthal-cut config the controller reads each frame (mirrors
    /// the `cfg` fields; kept as plain data so the controller carries no app_state
    /// dependency). Angles are `float` to match the cfg fields exactly so the
    /// resident-cut freshness comparison is bit-identical to the old inline one.
    struct AngleCut {
        bool enabled{false};
        float start_deg{0.f};
        float end_deg{0.f};
    };

    struct Callbacks {
        /// A fresh uncut base scene finished tessellating → upload + frame it.
        std::function<void(std::shared_ptr<const RenderScene>)> on_base_scene_ready;
        /// A Boolean-cut bake finished → upload it into the cut renderer.
        std::function<void(std::shared_ptr<const RenderScene>)> on_cut_scene_ready;
        /// A new project build is starting → invalidate the resident cut bake
        /// (drop `cut_scene`, its upload flag, and clear the cut renderer).
        std::function<void()> on_project_build_starting;
    };

    void configure(ui::Notifications *notifications, Callbacks callbacks);

    /// Drive the project walk, the session, and the in-flight build job once per
    /// frame. `cut` is the live cut config; `cut_uploaded` is whether the App
    /// currently has a GPU-resident cut (needed for the freshness check).
    void poll(ProjectFs *project, const AngleCut &cut, bool cut_uploaded);

    /// Queue a Boolean-cut (re)build for the next poll (toggle / angle commit).
    void requestCutRebuild() { pending_cut_rebuild_ = true; }

    void setRootConfigKey(std::string key);
    void setRootGeometryKey(std::string key);
    void setRootKeys(std::string config_key, std::string geometry_key);
    void clearError() { error_.clear(); }

    /// Drop the pristine cache, root keys, session keys, the pending-cut flag,
    /// and the error string (project close). Leaves any in-flight job to drain.
    void reset();

    // Accessors for the UI context, render()'s cut selection, and jobsRunning().
    [[nodiscard]] SceneBuildJob &job() { return job_; }
    [[nodiscard]] BuildSession &session() { return session_; }
    [[nodiscard]] const BuildSession &session() const { return session_; }
    [[nodiscard]] std::string &rootConfigKey() { return root_config_key_; }
    [[nodiscard]] std::string &rootGeometryKey() { return root_geometry_key_; }
    [[nodiscard]] std::string &error() { return error_; }
    [[nodiscard]] bool inProgress() const { return in_progress_; }
    [[nodiscard]] bool pendingCutRebuild() const { return pending_cut_rebuild_; }
    [[nodiscard]] float cutBuiltStartDeg() const { return cut_built_start_deg_; }
    [[nodiscard]] float cutBuiltEndDeg() const { return cut_built_end_deg_; }

  private:
    void startBuild(std::shared_ptr<const NHConfig> config,
                    std::shared_ptr<const SemanticScene> scene, std::string config_label,
                    std::string geometry_label, std::optional<WedgeCutParams> wedge,
                    const AngleCut &cut);
    void updateProgress();

    ui::Notifications *notifications_{nullptr};
    Callbacks cb_;

    SceneBuildJob job_;
    BuildSession session_;
    bool in_progress_{false};
    std::chrono::steady_clock::time_point start_time_{};

    // Pristine (pre-cut) build inputs, cached on each fresh session build so
    // toggling / re-aiming the cut re-derives from uncut geometry without
    // re-walking the project.
    std::shared_ptr<const NHConfig> pristine_config_;
    std::shared_ptr<const SemanticScene> pristine_scene_;
    std::string pristine_config_label_;
    std::string pristine_geometry_label_;
    /// Input hash of the last base scene we successfully tessellated. When a
    /// fresh walk resolves byte-identical inputs (e.g. after promoting a project
    /// to an archive, which re-seeds the same keys against a new backend), the
    /// build is skipped and the live scene + cut bake are kept. 0 until the first
    /// successful build. `in_flight_input_hash_` holds the hash of the build
    /// currently tessellating; it is promoted to `last_built_` only once that
    /// base build lands, so a failed tessellation doesn't suppress a retry.
    std::uint64_t last_built_input_hash_{0};
    std::uint64_t in_flight_input_hash_{0};

    bool building_cut_{false};        ///< the in-flight build is a cut (vs base) build
    bool pending_cut_rebuild_{false}; ///< a cut (re)build was requested
    float cut_built_start_deg_{0.f};  ///< angle the resident cut scene was built at
    float cut_built_end_deg_{0.f};
    float in_flight_cut_start_deg_{0.f}; ///< angle the in-flight cut build is using
    float in_flight_cut_end_deg_{0.f};

    ui::Notifications::ProgressHandle progress_handle_{0};
    std::string error_;
    std::string root_config_key_;
    std::string root_geometry_key_;
};

} // namespace nodehammer::viewer
