#include "view_panel.hpp"

#include "../ibl.hpp"
#include "../scene_renderer.hpp"
#include <nodehammer/viewer/camera.hpp>
#include <nodehammer/viewer/render_quality.hpp>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <algorithm>
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

// Toward-sun direction from azimuth/elevation in degrees. Y is up; azimuth
// rotates around Y from +Z (north) toward +X (east).
glm::vec3 sphericalToDir(float az_deg, float el_deg) {
    const float az = glm::radians(az_deg);
    const float el = glm::radians(el_deg);
    const float ce = std::cos(el);
    return {ce * std::sin(az), std::sin(el), ce * std::cos(az)};
}

void dirToSpherical(const glm::vec3 &dir, float &az_deg, float &el_deg) {
    const glm::vec3 d = glm::length(dir) > 0.f ? glm::normalize(dir) : glm::vec3{0.f, 1.f, 0.f};
    el_deg = glm::degrees(std::asin(std::clamp(d.y, -1.f, 1.f)));
    az_deg = glm::degrees(std::atan2(d.x, d.z));
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
    auto request_rebuild = [&]() {
        if (actions.request_scene_rebuild) {
            actions.request_scene_rebuild();
        }
    };

    // The Boolean cut produces real watertight cut faces but needs a full
    // re-tessellation, so toggling it (or committing a new angle below) kicks
    // off an async rebuild rather than updating live like the shader cut.
    if (ImGui::Checkbox("boolean cut", &ctx.cfg.boolean_cut)) {
        request_rebuild();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(rebuilds)");

    // When the Boolean cut is on, the shader cut is driven automatically (it
    // previews while dragging, then the bake takes over once the angle settles),
    // so the manual shader-cut toggles don't apply — grey them out.
    ImGui::BeginDisabled(ctx.cfg.boolean_cut);
    ImGui::Checkbox("angle cut", &ctx.cfg.angle_cut);
    ImGui::Checkbox("shader angle cut", &ctx.cfg.shader_angle_cut);
    ImGui::EndDisabled();

    // Mid-drag the shader cut previews the angle; on release/commit we rebuild
    // the Boolean cut at the committed angle (only when boolean cut is enabled).
    bool cut_committed = false;
    ImGui::SliderFloat("cut start", &ctx.cfg.angle_cut_start_deg, 0.f, 360.f, "%.1f deg");
    cut_committed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::InputFloat("##cut_start_input", &ctx.cfg.angle_cut_start_deg, 1.f, 15.f, "%.1f")) {
        ctx.cfg.angle_cut_start_deg = wrapDegrees(ctx.cfg.angle_cut_start_deg);
    }
    cut_committed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SliderFloat("cut end", &ctx.cfg.angle_cut_end_deg, 0.f, 360.f, "%.1f deg");
    cut_committed |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::InputFloat("##cut_end_input", &ctx.cfg.angle_cut_end_deg, 1.f, 15.f, "%.1f")) {
        ctx.cfg.angle_cut_end_deg = wrapDegrees(ctx.cfg.angle_cut_end_deg);
    }
    cut_committed |= ImGui::IsItemDeactivatedAfterEdit();

    if (ctx.cfg.boolean_cut && cut_committed) {
        request_rebuild();
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
            ImGui::SetItemTooltip("Fast Approximate Anti-Aliasing "
                                  "(post-process, PC-quality variant)");

            // Quality knobs only bite when FXAA is on.
            ImGui::BeginDisabled(!ctx.quality.enable_fxaa);
            const char *kFxaaQualityLabels[] = {"low", "medium", "high", "ultra"};
            int fxaa_q = static_cast<int>(ctx.quality.fxaa_quality);
            if (ImGui::Combo("FXAA quality", &fxaa_q, kFxaaQualityLabels,
                             IM_ARRAYSIZE(kFxaaQualityLabels))) {
                ctx.quality.fxaa_quality = static_cast<FxaaQualityPreset>(fxaa_q);
            }
            ImGui::SetItemTooltip("Edge-search step budget. Higher resolves longer "
                                  "near-horizontal/vertical edges at linear cost.");
            ImGui::SliderFloat("FXAA subpix", &ctx.quality.fxaa_subpix, 0.0f, 1.0f, "%.2f");
            ImGui::SetItemTooltip("Sub-pixel softening. 0 = edge-only (sharpest, thin "
                                  "features can shimmer); 1 = max smoothing. 0.75 = default.");
            ImGui::SliderFloat("FXAA edge", &ctx.quality.fxaa_edge_threshold, 0.063f, 0.333f,
                               "%.3f");
            ImGui::SetItemTooltip("Relative contrast needed to treat a pixel as an edge. "
                                  "Lower catches fainter edges (can soften texture).");
            ImGui::SliderFloat("FXAA edge min", &ctx.quality.fxaa_edge_threshold_min, 0.0312f,
                               0.0833f, "%.4f");
            ImGui::SetItemTooltip("Absolute luma floor — ignore edges in near-black regions.");
            ImGui::EndDisabled();

            ImGui::EndDisabled();
        }

        // Background dome — sample the IBL prefilter cubemap as the visible
        // sky on pixels that haven't been written by scene geometry. Greyed
        // out under depth-debug for the same reason as FXAA/AO (composite
        // short-circuits to depth visualization in those modes).
        {
            const bool depth_debug = (ctx.quality.debug_view != DebugView::Off);
            ImGui::BeginDisabled(depth_debug);
            ImGui::Checkbox("background", &ctx.quality.enable_background);
            ImGui::SetItemTooltip("Show the IBL sky as the visible background "
                                  "(matches the reflected sky on metallics)");
            ImGui::EndDisabled();
        }

        // GTAO. Sub-controls collapse entirely when the master AO toggle
        // is off — saves panel real estate and makes "is this knob active?"
        // unambiguous (vs the BeginDisabled grey-out pattern, which leaves
        // sliders visible but un-clickable). Bent-strength has the same
        // pattern nested under enable_advanced_ao.
        {
            const bool depth_debug = (ctx.quality.debug_view != DebugView::Off);
            ImGui::BeginDisabled(depth_debug);
            ImGui::Checkbox("AO", &ctx.quality.enable_ao);
            ImGui::SetItemTooltip("Screen-space ambient occlusion (GTAO, depth-only)");
            if (ctx.quality.enable_ao) {
                ImGui::Indent();
                ImGui::SliderFloat("intensity", &ctx.quality.ao_intensity, 0.f, 2.f, "%.2f");
                ImGui::SliderFloat("radius", &ctx.quality.ao_radius, 0.f, 1.f, "%.2f");
                ImGui::SliderFloat("thickness", &ctx.quality.ao_thickness, 0.1f, 4.f, "%.2f");
                ImGui::SetItemTooltip("Reject horizon samples farther than this many radii away "
                                      "(reduces silhouette fringe)");
                // Sample-count preset. Drives (slices, steps) in the GTAO
                // FS; see AoPass::draw for the per-preset values. Higher =
                // less jitter at linear cost.
                static const char *kAoQualityLabels[] = {"Low (4×3)", "Medium (4×4)", "High (6×6)",
                                                         "Ultra (8×8)"};
                int ao_quality_idx = static_cast<int>(ctx.quality.ao_quality);
                if (ImGui::Combo("samples", &ao_quality_idx, kAoQualityLabels,
                                 IM_ARRAYSIZE(kAoQualityLabels))) {
                    ctx.quality.ao_quality = static_cast<AoQualityPreset>(ao_quality_idx);
                }
                ImGui::SetItemTooltip("GTAO sample count per pixel — slices × steps. More samples\n"
                                      "= less per-pixel jitter before the denoise pass eats it.");
                // AO render resolution. The GTAO + denoise passes are fullscreen
                // and were the single biggest GPU cost at full res; AO is
                // low-frequency so half-res reclaims most of it. Bilinearly
                // upsampled by the scene shader / composite.
                ImGui::SliderFloat("resolution", &ctx.quality.ao_resolution_scale, 0.25f, 1.0f,
                                   "%.2fx");
                ImGui::SetItemTooltip(
                    "Fraction of scene resolution for the GTAO + denoise passes.\n"
                    "0.5 = quarter the pixels — big GPU win for little visible\n"
                    "loss (AO is low-frequency). 1.0 = full res.");
                // Denoise toggle. Strict quality win when on; exposed so
                // the user can A/B the raw GTAO jitter vs the denoised
                // result.
                ImGui::Checkbox("denoise", &ctx.quality.enable_ao_denoise);
                ImGui::SetItemTooltip("Bilateral 5×5 depth-aware denoise on the raw GTAO\n"
                                      "(off = scene/composite sample the raw noisy AO)");
                // Advanced AO toggle and bent-normal strength. Advanced
                // wires bent normal + multi-bounce + specular occlusion
                // into the scene shader's PBR IBL term. Off → composite
                // does the legacy single-multiply on the AO scalar.
                ImGui::Checkbox("advanced (PBR)", &ctx.quality.enable_advanced_ao);
                ImGui::SetItemTooltip("On: scene PBR applies multi-bounce, bent-normal IBL,\n"
                                      "and specular occlusion. Off: composite single-multiply.\n"
                                      "Only affects the PBR shading path.");
                if (ctx.quality.enable_advanced_ao) {
                    ImGui::Indent();
                    ImGui::SliderFloat("bent strength", &ctx.quality.ao_bent_strength, 0.f, 1.f,
                                       "%.2f");
                    ImGui::SetItemTooltip(
                        "Blend bent normal toward N. 0 = no bent-normal effect\n"
                        "(multi-bounce + SO still apply); 1 = full bent normal.\n"
                        "Pull down if cavities look noisy or IBL drifts with camera.");
                    ImGui::Unindent();
                }
                ImGui::Unindent();
            }
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

        // Tonemap curve combo only makes sense when tonemap is enabled —
        // collapse the row when off rather than grey-greying out, matching
        // the AO/advanced-AO pattern above.
        if (ctx.quality.enable_tonemap) {
            const char *kTonemapLabels[] = {"ACES", "Reinhard", "AgX"};
            int tm_idx = static_cast<int>(ctx.quality.tonemap_mode);
            ImGui::Indent();
            if (ImGui::Combo("tonemap curve", &tm_idx, kTonemapLabels,
                             IM_ARRAYSIZE(kTonemapLabels))) {
                ctx.quality.tonemap_mode = static_cast<TonemapMode>(tm_idx);
            }
            ImGui::Unindent();
        }

        // Pre-tonemap "look" knobs. Apply in linear HDR space regardless
        // of whether the tonemap is on, so they remain useful for raw
        // passthrough viewing too. Defaults of 1.0 are no-ops.
        ImGui::SliderFloat("contrast", &ctx.quality.contrast, 0.5f, 2.0f, "%.2f");
        ImGui::SetItemTooltip("Pre-tonemap pow around 0.18 mid-gray. >1 punches contrast.");
        ImGui::SliderFloat("saturation", &ctx.quality.saturation, 0.0f, 2.0f, "%.2f");
        ImGui::SetItemTooltip("0 = grayscale, 1 = identity, >1 boosts color.");
        ImGui::SameLine();
        if (ImGui::SmallButton("reset##look")) {
            ctx.quality.contrast = 1.0f;
            ctx.quality.saturation = 1.0f;
        }

        // Render scale is implemented: it sizes the offscreen scene/AO/denoise
        // targets and the composite upsamples to the window — the primary
        // fragment-cost lever. <1 trades sharpness for speed; >1 supersamples.
        ImGui::SliderFloat("render scale", &ctx.quality.render_scale, 0.5f, 2.0f, "%.2fx");
        ImGui::SetItemTooltip("Offscreen render resolution = window x scale. Lower = fewer "
                              "fragments across every pass (scene, AO, denoise);\n"
                              "the composite upsamples to the window.");

        ImGui::Checkbox("cap FPS to 60", &ctx.quality.cap_fps);
        ImGui::SetItemTooltip("Skip frames to hold the render rate at ~60 FPS.\n"
                              "Useful on high-refresh (120Hz+) displays to save power.");

        // The remaining controls advertise plumbing for bloom / IBL-quality
        // that lands in later phases. Disabled today so they show what's
        // coming without misleading the user.
        ImGui::BeginDisabled(true);
        ImGui::Checkbox("bloom", &ctx.quality.enable_bloom);
        ImGui::SliderInt("IBL quality", &ctx.quality.ibl_quality, 0, 3);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("wired, not implemented");
        }
    }

    if (ctx.ibl_settings != nullptr && ImGui::CollapsingHeader("Lighting")) {
        // Single source of truth: edits here flip ibl_settings, which is the
        // IBL cache key (operator==), so the debounced rebake loop in
        // App::onFrame picks up changes after a 300 ms settle. The same
        // sun_dir feeds the analytical scene light (docs §9.1).
        IblSettings &s = *ctx.ibl_settings;

        const char *kSkyModelLabels[] = {"gradient (legacy)", "Nishita (atmospheric)"};
        int sky_idx = static_cast<int>(s.sky_model);
        if (ImGui::Combo("sky model", &sky_idx, kSkyModelLabels, IM_ARRAYSIZE(kSkyModelLabels))) {
            s.sky_model = static_cast<SkyModel>(sky_idx);
        }

        float az_deg = 0.f, el_deg = 0.f;
        dirToSpherical(s.sun_dir, az_deg, el_deg);
        bool sun_changed = false;
        sun_changed |= ImGui::SliderFloat("sun azimuth", &az_deg, -180.f, 180.f, "%.1f deg");
        sun_changed |= ImGui::SliderFloat("sun elevation", &el_deg, -10.f, 90.f, "%.1f deg");
        if (sun_changed) {
            s.sun_dir = sphericalToDir(az_deg, el_deg);
        }
        ImGui::SliderFloat("sun intensity", &s.sun_intensity, 0.f, 20.f, "%.2f");

        const bool nishita = (s.sky_model == SkyModel::Nishita);
        ImGui::BeginDisabled(!nishita);
        ImGui::SliderFloat("turbidity", &s.turbidity, 1.5f, 10.f, "%.2f");
        ImGui::SetItemTooltip(
            "Atmospheric haze. ~2 = clear sky, ~6 = hazy. Drives Mie scattering.");
        ImGui::ColorEdit3("ground albedo", &s.ground_albedo.x);
        ImGui::EndDisabled();

        // Reset only the sky/sun-related fields; preserve user-tuned bake
        // sample counts (those live under "IBL bake" in the debug panel).
        if (ImGui::Button("Reset sky")) {
            const IblSettings def{};
            s.sky_model = def.sky_model;
            s.sun_dir = def.sun_dir;
            s.sun_color = def.sun_color;
            s.sun_intensity = def.sun_intensity;
            s.sun_sharpness = def.sun_sharpness;
            s.zenith_color = def.zenith_color;
            s.horizon_color = def.horizon_color;
            s.ground_color = def.ground_color;
            s.turbidity = def.turbidity;
            s.ground_albedo = def.ground_albedo;
        }
    }

    ImGui::End();
}

} // namespace nodehammer::viewer::ui
