#include "status_ui.hpp"

#include <nodehammer/viewer/build_session.hpp>
#include <nodehammer/viewer/platform.hpp>
#include <nodehammer/viewer/project_fs.hpp>

#include <imgui.h>

namespace nodehammer::viewer::ui {

void renderStatusWindow(bool *open, const ViewerUiContext &ctx, const UiActions &actions) {
    if (!ImGui::Begin("Status", open)) {
        ImGui::End();
        return;
    }

    if (!ctx.has_scene) {
        bool show_drag_hint = true;
        if (ctx.project != nullptr) {
            if (ctx.project->status() == ProjectFsStatus::Error) {
                ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "Asset load failed");
                show_drag_hint = false;
            }

            if (ctx.build_in_progress) {
                show_drag_hint = false;
            }

            const bool has_files = !ctx.project->list("").empty();
            if (has_files) {
                show_drag_hint = false;
            }

            if (ctx.build_session.phase() == BuildPhase::WaitingForUser) {
                show_drag_hint = false;
                for (const auto &key : ctx.build_session.missing()) {
                    ImGui::Text("Still need: %s", key.c_str());
                }
            } else if (ctx.build_session.phase() == BuildPhase::Error) {
                show_drag_hint = false;
                ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "Build session error");
            }

            if (has_files) {
                if (ctx.root_config_key.empty()) {
                    ImGui::Text("Pick a .toml config in the Project window");
                }
                if (ctx.root_geometry_key.empty()) {
                    ImGui::Text(
                        "Pick a FlatBuffer geometry (.nhb / .nhb.zst) in the Project window");
                }
            }

            for (const auto &warning : ctx.project->warnings()) {
                ImGui::TextColored({0.9f, 0.85f, 0.4f, 1.f}, "%s", warning.c_str());
            }
        }

        if (!ctx.build_error.empty()) {
            ImGui::TextColored({1.f, 0.4f, 0.4f, 1.f}, "%s", ctx.build_error.c_str());
        }
        if (show_drag_hint) {
            ImGui::Text("Drag a config (.toml) and a geometry file onto the window,");
            ImGui::Text("or use the Open files button below.");
        }

        if (ImGui::Button("Open files...") && actions.open_file_picker) {
            actions.open_file_picker();
        }
        if constexpr (!platform::kIsWeb) {
            ImGui::SameLine();
            if (ImGui::Button("Open folder...") && actions.open_folder_picker) {
                actions.open_folder_picker();
            }
        }
    } else if (!ctx.scene_uploaded) {
        ImGui::Text("Uploading scene to GPU...");
        ImGui::ProgressBar(-1.f * static_cast<float>(ImGui::GetTime()), ImVec2(-1.f, 0.f), "");
    } else {
        ImGui::Text("Ready");
    }

    ImGui::End();
}

} // namespace nodehammer::viewer::ui
