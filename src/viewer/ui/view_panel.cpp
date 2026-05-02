#include "view_panel.hpp"

#include "../scene_renderer.hpp"
#include <nodehammer/viewer/camera.hpp>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <cmath>

namespace nodehammer::viewer::ui {
namespace {

float wrapDegrees(float angle) {
    angle = std::fmod(angle, 360.f);
    if (angle < 0.f) {
        angle += 360.f;
    }
    return angle;
}

} // namespace

void renderViewPanel(bool *open, const ViewerUiContext &ctx, const UiActions &actions) {
    if (!ImGui::Begin("View", open)) {
        ImGui::End();
        return;
    }

    if (!ctx.has_scene) {
        ImGui::TextDisabled("(no scene loaded)");
        ImGui::End();
        return;
    }

    ImGui::Text("Meshes: %u", ctx.scene_renderer.meshAssetCount());
    ImGui::Text("Nodes: %u", ctx.scene_renderer.nodeCount());
    ImGui::Text("Tris (scene): %llu",
                static_cast<unsigned long long>(ctx.scene_renderer.triangleCount()));
    const auto fs = ctx.scene_renderer.lastFrameStats();
    ImGui::Text("Draw calls: %u  Instances: %u  Tris/frame: %llu", fs.draw_calls, fs.instances,
                static_cast<unsigned long long>(fs.triangles));

    if (ImGui::Button("Frame scene") && actions.frame_scene) {
        actions.frame_scene();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close project") && actions.close_project) {
        actions.close_project();
    }

    ImGui::Separator();
    ImGui::Checkbox("backface cull", &ctx.cfg.cull_back);
    ImGui::Checkbox("auto orbit", &ctx.cfg.auto_orbit);
    ImGui::SliderFloat("orbit speed", &ctx.cfg.auto_orbit_speed_deg, -90.f, 90.f, "%.1f deg/s");
    ImGui::Checkbox("angle cut", &ctx.cfg.angle_cut);
    ImGui::Checkbox("shader angle cut", &ctx.cfg.shader_angle_cut);
    ImGui::SliderFloat("cut start", &ctx.cfg.angle_cut_start_deg, 0.f, 360.f, "%.1f deg");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::InputFloat("##cut_start_input", &ctx.cfg.angle_cut_start_deg, 1.f, 15.f, "%.1f")) {
        ctx.cfg.angle_cut_start_deg = wrapDegrees(ctx.cfg.angle_cut_start_deg);
    }
    ImGui::SliderFloat("cut end", &ctx.cfg.angle_cut_end_deg, 0.f, 360.f, "%.1f deg");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::InputFloat("##cut_end_input", &ctx.cfg.angle_cut_end_deg, 1.f, 15.f, "%.1f")) {
        ctx.cfg.angle_cut_end_deg = wrapDegrees(ctx.cfg.angle_cut_end_deg);
    }

    ImGui::Separator();
    ImGui::Checkbox("PBR / IBL", &ctx.cfg.enable_pbr);
    ImGui::Text("Camera: yaw=%.1f pitch=%.1f dist=%.2f", glm::degrees(ctx.camera.yaw),
                glm::degrees(ctx.camera.pitch), ctx.camera.distance);
    ImGui::Text("        near=%.3f far=%.1f", ctx.camera.near_plane, ctx.camera.far_plane);

    ImGui::End();
}

} // namespace nodehammer::viewer::ui
