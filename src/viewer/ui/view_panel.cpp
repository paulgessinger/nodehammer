#include "view_panel.hpp"

#include "../scene_renderer.hpp"
#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/render_quality.hpp>

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
    {
        // Tri-state cull control. `Auto` is the correct-behavior default
        // (per-material `doubleSided` decides); `ForceCull` / `ForceNoCull`
        // are debug overrides that ignore the material flag globally.
        const char *items[] = {"auto (per material)", "force on", "force off"};
        int current = static_cast<int>(ctx.cfg.cull);
        if (ImGui::Combo("backface cull", &current, items, IM_ARRAYSIZE(items))) {
            ctx.cfg.cull = static_cast<CullOverride>(current);
        }
    }
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
    int projection_idx = ctx.camera.projection == ProjectionMode::Orthographic ? 1 : 0;
    if (ImGui::RadioButton("perspective", &projection_idx, 0)) {
        ctx.camera.projection = ProjectionMode::Perspective;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("orthographic", &projection_idx, 1)) {
        ctx.camera.projection = ProjectionMode::Orthographic;
    }
    ImGui::Text("Camera: yaw=%.1f pitch=%.1f dist=%.2f", glm::degrees(ctx.camera.yaw),
                glm::degrees(ctx.camera.pitch), ctx.camera.distance);
    ImGui::Text("        near=%.3f far=%.1f", ctx.camera.near_plane, ctx.camera.far_plane);

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Render Quality")) {
        const char *kDebugViewLabels[] = {"off", "depth (raw)", "depth (linear)"};
        int debug_idx = static_cast<int>(ctx.quality.debug_view);
        if (ImGui::Combo("debug view", &debug_idx, kDebugViewLabels,
                         IM_ARRAYSIZE(kDebugViewLabels))) {
            ctx.quality.debug_view = static_cast<DebugView>(debug_idx);
        }

        // FXAA is live; greyed out only while a depth debug view is active
        // because the composite FS short-circuits FXAA in those modes.
        {
            const bool depth_debug = (ctx.quality.debug_view != DebugView::Off);
            ImGui::BeginDisabled(depth_debug);
            ImGui::Checkbox("FXAA", &ctx.quality.enable_fxaa);
            ImGui::SetItemTooltip("Fast Approximate Anti-Aliasing (post-process)");
            ImGui::EndDisabled();
        }

        // GTAO. Same depth-debug grey-out as FXAA; intensity/radius nest-
        // disable on the AO checkbox so the user can't tweak invisibly.
        {
            const bool depth_debug = (ctx.quality.debug_view != DebugView::Off);
            ImGui::BeginDisabled(depth_debug);
            ImGui::Checkbox("AO", &ctx.quality.enable_ao);
            ImGui::SetItemTooltip("Screen-space ambient occlusion (GTAO, depth-only)");
            ImGui::BeginDisabled(!ctx.quality.enable_ao);
            ImGui::SliderFloat("AO intensity", &ctx.quality.ao_intensity, 0.f, 2.f, "%.2f");
            ImGui::SliderFloat("AO radius", &ctx.quality.ao_radius, 0.f, 1.f, "%.2f");
            ImGui::SliderFloat("AO thickness", &ctx.quality.ao_thickness, 0.1f, 4.f, "%.2f");
            ImGui::SetItemTooltip("Reject horizon samples farther than this many radii away "
                                  "(reduces silhouette fringe)");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
        }

        // HDR + tonemap are live. HDR greys out on backends that don't
        // expose RGBA16F as render+blend (e.g. WebGL2 without
        // EXT_color_buffer_half_float).
        {
            ImGui::BeginDisabled(!ctx.hdr_supported);
            ImGui::Checkbox("HDR", &ctx.quality.enable_hdr);
            ImGui::SetItemTooltip(ctx.hdr_supported
                                      ? "Render scene into RGBA16F for higher highlight range"
                                      : "RGBA16F not renderable on this backend");
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::Checkbox("tonemap", &ctx.quality.enable_tonemap);
        ImGui::SetItemTooltip("Apply exposure + curve in composite (most useful with HDR on)");

        ImGui::SliderFloat("exposure", &ctx.quality.exposure_stops, -4.f, 4.f, "%+.1f stops");

        {
            const char *kTonemapLabels[] = {"ACES", "Reinhard", "AgX"};
            int tm_idx = static_cast<int>(ctx.quality.tonemap_mode);
            ImGui::BeginDisabled(!ctx.quality.enable_tonemap);
            if (ImGui::Combo("tonemap curve", &tm_idx, kTonemapLabels,
                             IM_ARRAYSIZE(kTonemapLabels))) {
                ctx.quality.tonemap_mode = static_cast<TonemapMode>(tm_idx);
            }
            ImGui::EndDisabled();
        }

        // The remaining controls advertise the plumbing for MSAA/bloom/
        // render-scale/IBL-quality that lands in later phases. Disabled
        // today so they show what's coming without misleading the user.
        ImGui::BeginDisabled(true);
        ImGui::SliderFloat("render scale", &ctx.quality.render_scale, 0.5f, 2.0f, "%.2fx");
        ImGui::Checkbox("bloom", &ctx.quality.enable_bloom);
        ImGui::SliderInt("MSAA", &ctx.quality.msaa_samples, 1, 8);
        ImGui::SliderInt("IBL quality", &ctx.quality.ibl_quality, 0, 3);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("wired, not implemented");
        }
    }

    ImGui::End();
}

} // namespace nodehammer::viewer::ui
