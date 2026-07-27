#include "build_controller.hpp"

#include <nodehammer/ir/diagnostics.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <iostream>
#include <limits>
#include <print>
#include <string>
#include <utility>

namespace nodehammer::viewer {

void BuildController::configure(ui::Notifications *notifications, Callbacks callbacks) {
    notifications_ = notifications;
    cb_ = std::move(callbacks);
}

void BuildController::setRootConfigKey(std::string key) {
    root_config_key_ = std::move(key);
    session_.setRootKeys(root_config_key_, root_geometry_key_);
    error_.clear();
}

void BuildController::setRootGeometryKey(std::string key) {
    root_geometry_key_ = std::move(key);
    session_.setRootKeys(root_config_key_, root_geometry_key_);
    error_.clear();
}

void BuildController::setRootKeys(std::string config_key, std::string geometry_key) {
    root_config_key_ = std::move(config_key);
    root_geometry_key_ = std::move(geometry_key);
    session_.setRootKeys(root_config_key_, root_geometry_key_);
}

void BuildController::reset() {
    pristine_scene_.reset();
    pristine_config_.reset();
    pristine_config_label_.clear();
    pristine_geometry_label_.clear();
    last_built_input_hash_ = 0;
    root_config_key_.clear();
    root_geometry_key_.clear();
    session_.setRootKeys({}, {});
    pending_cut_rebuild_ = false;
    error_.clear();
}

void BuildController::startBuild(std::shared_ptr<const NHConfig> config,
                                 std::shared_ptr<const SemanticScene> scene,
                                 std::string config_label, std::string geometry_label,
                                 std::optional<WedgeCutParams> wedge, const AngleCut &cut) {
    start_time_ = std::chrono::steady_clock::now();
    building_cut_ = wedge.has_value();
    if (building_cut_) {
        in_flight_cut_start_deg_ = cut.start_deg;
        in_flight_cut_end_deg_ = cut.end_deg;
    }
    job_.start(std::move(config), std::move(scene), std::move(config_label),
               std::move(geometry_label), wedge);
    in_progress_ = true;
    if (notifications_ != nullptr) {
        progress_handle_ =
            notifications_->startProgress(building_cut_ ? "Applying cut..." : "Tessellating...");
    }
}

void BuildController::updateProgress() {
    if (progress_handle_ == 0 || notifications_ == nullptr) {
        return;
    }
    std::string label;
    float frac = 0.0f;
    if (job_.phase() == SceneBuildJob::Phase::Cutting) {
        // Cooperative wedge cut — bar the placement-classification sweep.
        const auto total = job_.wedgeCutTotal();
        const auto processed = job_.wedgeCutProcessed();
        frac = total > 0 ? static_cast<float>(processed) / static_cast<float>(total) : 0.0f;
        label = total > 0 ? std::format("Applying cut ({}/{} placements)", processed, total)
                          : std::string{"Applying cut..."};
    } else {
        const auto total = job_.tessellationTotal();
        const auto processed = job_.tessellationProcessed();
        frac = total > 0 ? static_cast<float>(processed) / static_cast<float>(total) : 0.0f;
        if (total > 0) {
            label = std::format("Tessellating ({}/{} nodes)", processed, total);
        }
    }
    notifications_->updateProgress(progress_handle_, frac, label);
}

void BuildController::poll(ProjectFs *project, const AngleCut &cut, bool cut_uploaded) {
    // Drive the off-loop tessellation. On native this is a poll of an atomic
    // flag set by the worker thread; on web it runs the build synchronously on
    // the second poll (the first paints a "Tessellating…" frame).
    if (in_progress_) {
        updateProgress();
    }
    if (in_progress_ && job_.poll()) {
        auto built = job_.take();
        for (const auto &d : built.diags.items()) {
            std::println(stderr, "scene_build: {} {}", d.code, d.message);
            if (notifications_ != nullptr) {
                notifications_->diagnostic(d);
            }
        }
        if (built.scene) {
            const auto build_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start_time_)
                                      .count();
            std::println("viewer: tessellation complete ({:.1f} ms, {} nodes, {} mesh assets, "
                         "{} materials)",
                         build_ms, built.scene->nodes.size(), built.scene->meshAssets.size(),
                         built.scene->materials.size());
            if (progress_handle_ != 0 && notifications_ != nullptr) {
                notifications_->finishProgress(progress_handle_, "Tessellation complete");
                progress_handle_ = 0;
            }
            if (building_cut_) {
                // Boolean-cut bake → cut renderer. Record the angle it was built
                // at (not the live cfg, which may have moved during the build) so
                // the render selector knows whether it's still fresh.
                cut_built_start_deg_ = in_flight_cut_start_deg_;
                cut_built_end_deg_ = in_flight_cut_end_deg_;
                if (cb_.on_cut_scene_ready) {
                    cb_.on_cut_scene_ready(std::move(built.scene));
                }
            } else {
                if (cb_.on_base_scene_ready) {
                    cb_.on_base_scene_ready(std::move(built.scene));
                }
                // This base scene is now resident; record the inputs it was built
                // from so a later walk resolving identical bytes can skip the
                // rebuild.
                last_built_input_hash_ = in_flight_input_hash_;
                // The freshly loaded base scene needs a cut bake if the Boolean
                // cut is already enabled (e.g. from persisted state / URL).
                if (cut.enabled) {
                    pending_cut_rebuild_ = true;
                }
            }
            error_.clear();
        } else {
            // Errors are already surfaced as toasts via diagnostic() above; stash
            // the first one for the persistent status-bar message.
            error_ = "scene build failed";
            for (const auto &d : built.diags.items()) {
                if (d.severity >= DiagnosticSeverity::Error) {
                    error_ = d.message;
                    break;
                }
            }
            if (progress_handle_ != 0 && notifications_ != nullptr) {
                notifications_->cancelProgress(progress_handle_);
                progress_handle_ = 0;
            }
        }
        in_progress_ = false;
    }

    // Drive the project + build pipeline unconditionally so switching the root
    // selection after a scene is already rendered still walks → parse → build
    // the new selection. The build-job completion above swaps the scene over
    // when the new build lands.
    if (project != nullptr) {
        project->poll();
        session_.poll(project);

        if (!in_progress_ && session_.phase() == BuildPhase::ResolvedReady) {
            if (auto inputs = session_.takeInputs()) {
                // Byte-identical inputs to the resident scene (e.g. a backend
                // swap that resolves the same content, like promoting a project
                // to an archive): the parse → import → tessellate pipeline is
                // deterministic, so keep the live scene and cut bake untouched
                // instead of re-tessellating an identical result. Refresh only
                // the pristine labels the cut path uses.
                if (pristine_scene_ && inputs->input_hash == last_built_input_hash_) {
                    pristine_config_label_ = std::move(inputs->config_key);
                    pristine_geometry_label_ = std::move(inputs->geometry_key);
                } else {
                    // Cache pristine (uncut) inputs so cut bakes re-derive cleanly.
                    pristine_config_ =
                        std::make_shared<const NHConfig>(std::move(inputs->config.config));
                    pristine_scene_ =
                        std::make_shared<const SemanticScene>(std::move(inputs->import.scene));
                    pristine_config_label_ = std::move(inputs->config_key);
                    pristine_geometry_label_ = std::move(inputs->geometry_key);
                    // Promoted to last_built_input_hash_ once this base build
                    // lands successfully (see the completion handler).
                    in_flight_input_hash_ = inputs->input_hash;
                    // A new base build invalidates any resident cut bake (App drops
                    // the GPU-side cut scene + clears the cut renderer).
                    if (cb_.on_project_build_starting) {
                        cb_.on_project_build_starting();
                    }
                    pending_cut_rebuild_ = false;
                    // The base scene is always uncut (wedge = nullopt); the cut bake
                    // follows once the base lands (see the completion handler).
                    startBuild(pristine_config_, pristine_scene_, pristine_config_label_,
                               pristine_geometry_label_, std::nullopt, cut);
                }
            }
        }
    }

    // Boolean-cut (re)build: re-prep + re-tessellate the wedge cut from the
    // cached pristine scene (never from already-cut geometry). Skipped if a
    // resident cut already matches the committed angle.
    const bool cut_fresh =
        cut_uploaded && cut_built_start_deg_ == cut.start_deg && cut_built_end_deg_ == cut.end_deg;
    if (pending_cut_rebuild_ && !in_progress_ && pristine_scene_) {
        pending_cut_rebuild_ = false;
        if (cut.enabled && !cut_fresh) {
            startBuild(pristine_config_, pristine_scene_, pristine_config_label_,
                       pristine_geometry_label_, WedgeCutParams{cut.start_deg, cut.end_deg}, cut);
        }
    }
}

} // namespace nodehammer::viewer
