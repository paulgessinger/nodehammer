#include "viewer_ui.hpp"

#include "debug_panel.hpp"
#include "menu_bar.hpp"
#include "project_panel.hpp"
#include "status_ui.hpp"
#include "view_panel.hpp"

#include <viewer/platform.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <cfloat>

namespace nodehammer::viewer::ui {
namespace {

constexpr const char *kDockspaceName = "NodehammerDockspace";
constexpr const char *kProjectWindowName = "Project";
constexpr const char *kStatusWindowName = "Status";
constexpr const char *kDebugWindowName = "Debug";
constexpr const char *kViewWindowName = "View";
constexpr float kDefaultSidebarWidth = 300.f;

void buildDefaultDockLayout(ImGuiID dockspace_id, const ImVec2 &size) {
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    ImGuiID main_id = dockspace_id;
    const float sidebar_fraction =
        size.x > kDefaultSidebarWidth ? kDefaultSidebarWidth / size.x : 0.35f;
    const ImGuiID project_id =
        ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Left, sidebar_fraction, nullptr, &main_id);
    const ImGuiID right_stack_id =
        ImGui::DockBuilderSplitNode(main_id, ImGuiDir_Right, sidebar_fraction, nullptr, &main_id);

    ImGuiID middle_right_id = right_stack_id;
    const ImGuiID debug_id =
        ImGui::DockBuilderSplitNode(middle_right_id, ImGuiDir_Up, 0.25f, nullptr, &middle_right_id);
    const ImGuiID status_id = ImGui::DockBuilderSplitNode(middle_right_id, ImGuiDir_Down, 0.30f,
                                                          nullptr, &middle_right_id);

    ImGui::DockBuilderDockWindow(kProjectWindowName, project_id);
    ImGui::DockBuilderDockWindow(kDebugWindowName, debug_id);
    ImGui::DockBuilderDockWindow(kViewWindowName, middle_right_id);
    ImGui::DockBuilderDockWindow(kStatusWindowName, status_id);
    ImGui::DockBuilderFinish(dockspace_id);
}

void renderDockspace(UiState &state) {
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const ImVec2 pos = viewport->WorkPos;
    const ImVec2 size = viewport->WorkSize;

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags kHostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::Begin("Nodehammer Dockspace Host", nullptr, kHostFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspace_id = ImGui::GetID(kDockspaceName);
    if (!state.dockspace_built) {
        buildDefaultDockLayout(dockspace_id, size);
        state.dockspace_built = true;
    }

    ImGui::DockSpace(dockspace_id, ImVec2(0.f, 0.f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
}

void renderDragOverlay(const ViewerUiContext &ctx) {
    if (!ctx.platform_window_state.drag_hover.active) {
        return;
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float padding = 10.f;
    const ImVec2 min{viewport->Pos.x + padding, viewport->Pos.y + padding};
    const ImVec2 max{viewport->Pos.x + viewport->Size.x - padding,
                     viewport->Pos.y + viewport->Size.y - padding};
    auto *draw_list = ImGui::GetForegroundDrawList();
    const float rounding = 20.0f;
    draw_list->AddRectFilled(min, max, IM_COL32(80, 120, 180, 200), rounding);
    draw_list->AddRect(min, max, IM_COL32(130, 180, 255, 255), rounding, 0, 3.f);

    const char *message = ctx.platform_window_state.drag_hover.file_like
                              ? "Drop files to load them"
                              : "Drop supported scene files";
    ImFont *font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize() * 1.8f;
    const ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, message);
    const ImVec2 center{(min.x + max.x - text_size.x) * 0.5f, (min.y + max.y - text_size.y) * 0.5f};
    draw_list->AddText(font, font_size, {center.x + 1.f, center.y + 1.f}, IM_COL32(0, 0, 0, 180),
                       message);
    draw_list->AddText(font, font_size, center, IM_COL32(230, 240, 255, 255), message);
}

// Sync-to-URL window (web only): choose what steer to write into the address bar
// so a shared link reproduces the current screen, and whether to keep it live.
// [[maybe_unused]] — the call site is behind `if constexpr (kIsWeb)`, so on native
// this is compiled out and would otherwise warn as an unneeded internal decl.
[[maybe_unused]] void renderSyncPanel(UiState &state, const UiActions &actions) {
    if (!ImGui::Begin("Sync to URL", &state.show_sync)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Write the current view into the address bar so a shared link opens on the "
                       "exact same screen. Content must be published (a viewer-mode archive) for a "
                       "recipient to see it.");
    ImGui::Separator();
    ImGui::Checkbox("Keep the URL live (continuous)", &state.url_sync_continuous);
    ImGui::TextDisabled("What to include:");
    ImGui::Checkbox("Camera", &state.url_sync_camera);
    ImGui::Checkbox("View toggles & cut", &state.url_sync_view);
    ImGui::Separator();
    if (ImGui::Button("Copy link now") && actions.sync_browser_url) {
        actions.sync_browser_url();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("writes the URL");
    ImGui::End();
}

} // namespace

void renderViewerUi(UiState &state, const ViewerUiContext &ctx, const UiActions &actions) {
    renderDockspace(state);

    if (state.show_project) {
        renderProjectPanel(&state.show_project, ctx, actions);
    }
    if (state.show_status) {
        renderStatusWindow(&state.show_status, ctx, actions);
    }
    if (state.show_view) {
        renderViewPanel(&state.show_view, ctx, actions);
    }
    if (state.show_debug) {
        renderDebugPanel(&state.show_debug, ctx, actions);
    }
    if constexpr (platform::kIsWeb) {
        if (state.show_sync) {
            renderSyncPanel(state, actions);
        }
    }

    renderMenuBar(state, ctx, actions);
    renderDragOverlay(ctx);
}

} // namespace nodehammer::viewer::ui
