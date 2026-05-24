#include "menu_bar.hpp"

#include <nodehammer/viewer/platform.hpp>

#include <imgui.h>

namespace nodehammer::viewer::ui {

void renderMenuBar(UiState &state, const ViewerUiContext &ctx, const UiActions &actions) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open files...") && actions.open_file_picker) {
            actions.open_file_picker();
        }
        if constexpr (!platform::kIsWeb) {
            if (ImGui::MenuItem("Open folder...") && actions.open_folder_picker) {
                actions.open_folder_picker();
            }
        }
        if (ctx.has_scene && ImGui::MenuItem("Close project") && actions.close_project) {
            actions.close_project();
        }
        ImGui::Separator();
        const bool can_export = ctx.has_scene && ctx.scene_uploaded && !ctx.export_in_progress;
        if (ImGui::MenuItem("Export PNG...", nullptr, false, can_export) && actions.export_png) {
            actions.export_png();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        if (ctx.has_scene && ImGui::MenuItem("Frame scene") && actions.frame_scene) {
            actions.frame_scene();
        }
        ImGui::Separator();
        ImGui::MenuItem("Project", nullptr, &state.show_project);
        ImGui::MenuItem("Status", nullptr, &state.show_status);
        ImGui::MenuItem("View Controls", nullptr, &state.show_view);
        ImGui::MenuItem("Debug", nullptr, &state.show_debug);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            state.show_project = true;
            state.show_status = true;
            state.show_view = true;
            state.show_debug = true;
            state.dockspace_built = false;
        }
        ImGui::EndMenu();
    }

    if constexpr (platform::kIsWeb) {
        if (ImGui::BeginMenu("Web")) {
            if (ImGui::MenuItem("Commit settings to URL") && actions.sync_browser_url) {
                actions.sync_browser_url();
            }
            ImGui::EndMenu();
        }
    }

    ImGui::EndMainMenuBar();
}

} // namespace nodehammer::viewer::ui
