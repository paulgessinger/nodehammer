#pragma once

#include <nodehammer/viewer/config.hpp>

#include <glm/glm.hpp>
#include <memory>
#include <sokol_gfx.h>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

struct Camera;
struct IblBakeData;

/// GPU-side renderer for nodehammer's tessellated RenderScene IR using
/// sokol_gfx. Per-mesh-asset static vertex/index buffers, draws grouped by
/// (mesh, material) with hardware instancing for the per-node world matrix.
/// Lambertian shading; PBR comes later.
class SceneRenderer {
  public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer &) = delete;
    SceneRenderer &operator=(const SceneRenderer &) = delete;

    /// Initialise GPU resources (shader, pipelines, dummy IBL bindings).
    /// Idempotent. Call once after sg_setup so the renderer can produce a
    /// frame even before any scene is uploaded.
    void initialize();

    /// Set the offscreen scene-target color format the pipelines render into.
    /// Rebuilds the scene pipelines if the format actually changed (no-op
    /// otherwise). Required because WebGPU rejects render passes whose
    /// attachment format differs from the bound pipeline's declared
    /// `colors[0].pixel_format`. Call after `initialize()` and any time
    /// `App` switches between LDR and HDR offscreen targets.
    void setTargetColorFormat(sg_pixel_format fmt);

    /// Swap the placeholder IBL bindings for a real baked dataset. Safe to
    /// call any time after `initialize()`. Idempotent on the dummy state —
    /// re-installing replaces the previous IBL.
    void installIbl(const IblBakeData &data);

    /// Begin a chunked upload of `scene` to the GPU. Discards previous
    /// scene state. The renderer keeps `scene` alive until the upload
    /// completes (so the caller's shared_ptr can be released immediately).
    /// Pair with `advanceUpload`.
    void beginUpload(std::shared_ptr<const RenderScene> scene);

    /// Make progress on a chunked upload started by `beginUpload`. Spends
    /// up to `budget_ns` creating per-mesh GPU buffers and finalises the
    /// scene (materials, draw groups, instance buffer) on the call that
    /// processes the last mesh. Returns true once the upload is fully
    /// complete; further calls are no-ops until the next `beginUpload`.
    bool advanceUpload(uint64_t budget_ns = 8'000'000);

    /// Whether `advanceUpload` would still return false. Useful for UI
    /// feedback ("Uploading scene to GPU…").
    [[nodiscard]] bool uploadInProgress() const;

    /// Drop the currently-uploaded scene (per-mesh buffers, materials, draw
    /// groups, instance buffer). Keeps shaders / pipelines / IBL alive so
    /// the next `beginUpload` doesn't pay re-init cost. Idempotent.
    void clearScene();

    /// Release every sokol_gfx handle the renderer holds. Must be called
    /// before sg_shutdown — sokol asserts on outstanding resources at
    /// shutdown. Idempotent.
    void release();

    struct RenderFlags {
        bool wireframe{false};
        /// Tri-state cull control. `Auto` picks per-group cull from
        /// `RenderMaterial::doubleSided`; the two `Force*` variants are
        /// debug overrides that ignore the material flag globally.
        CullOverride cull{CullOverride::Auto};
        bool angle_cut{false};
        bool shader_angle_cut{true};
        float angle_cut_start_deg{0.f};
        float angle_cut_end_deg{90.f};
        bool enable_pbr{false}; ///< When on: Cook-Torrance + IBL ambient.

        /// Material-stack prefilter (viewer AA for sampling stacks): when on,
        /// the scene FS blends a merged-stack mesh's albedo toward the stack's
        /// area-weighted average (MeshAsset::stackAverage) as the pixel
        /// footprint outgrows the band width, band-limiting the cycling-
        /// material pattern that aliases into moire at distance. Untagged
        /// meshes are unaffected. Runtime A/B toggle.
        bool material_prefilter{false};
        /// Global multiplier on the stack-prefilter feature size (transition
        /// distance dial). See RenderQualitySettings::material_prefilter_scale.
        float material_prefilter_scale{1.0f};

        /// Per-distance hull LOD: merged stacks that carry an LOD proxy pick
        /// between their detailed slabs and the coarse convex-hull proxy per
        /// instance by projected screen size, cross-fading with a screen-door
        /// dither through a transition band (no pop). When off, the detailed
        /// slabs draw at all distances (the proxy is never shown).
        bool lod_hull_enable{true};
        /// Debug: force every LOD-proxy stack to its hull regardless of distance
        /// (the old global "hull LOD (preview)" swap) — for eyeballing the proxy
        /// look. Overrides lod_hull_enable when set.
        bool lod_hull_force{false};
        /// Projected screen size (px, ~bounding-diameter) at the midpoint of the
        /// detail<->hull cross-fade: a stack larger than this on screen draws
        /// detailed, smaller draws the hull.
        float lod_hull_screen_px{64.f};
        /// Half-width (px) of the cross-fade band around lod_hull_screen_px.
        /// Both representations draw and dither within [mid-band, mid+band].
        float lod_hull_band_px{24.f};

        /// Overdraw debug view. When on, every group draws through the
        /// additive-blend / no-depth / no-cull overdraw pipeline and the
        /// scene FS emits a constant per-fragment increment instead of
        /// shading, so the color target accumulates a per-pixel fragment
        /// count for the composite's overdraw heatmap. Overrides cull and
        /// shading; the angle cut still discards (cut fragments don't count).
        bool overdraw{false};

        /// Toward-sun direction for the analytical directional light. Should
        /// match the IBL bake's sun_dir so the analytical highlight aligns
        /// with the reflected sun in the cubemap (per docs §9.1).
        glm::vec3 sun_dir{0.4f, 0.7f, 0.6f};
        float sun_intensity{1.5f};
        // AO is consumed in the composite pass (current-frame), not the scene
        // shader — see composite_pass / app.cpp.
    };

    /// Submit draw calls for the active scene. Caller must have an active
    /// sg_begin_pass / sg_end_pass bracket around the call.
    void render(const Camera &camera, uint32_t fb_width, uint32_t fb_height, RenderFlags flags);

    /// Stats for the most recent render() call. Useful for the perf experiment.
    struct FrameStats {
        uint32_t draw_calls{0};
        uint32_t instances{0};
        uint64_t triangles{0};
    };
    [[nodiscard]] FrameStats lastFrameStats() const;

    /// World-space AABB of the loaded scene. Returns false if no scene is loaded.
    [[nodiscard]] bool worldBounds(glm::vec3 &min, glm::vec3 &max) const;

    [[nodiscard]] uint32_t meshAssetCount() const;
    [[nodiscard]] uint32_t nodeCount() const;
    [[nodiscard]] uint64_t triangleCount() const;

    /// Accessors for the installed IBL prefilter cubemap, so other passes
    /// (e.g. composite background dome) can sample the same baked sky the
    /// scene shader uses for specular reflections.
    [[nodiscard]] sg_view iblPrefilterView() const;
    [[nodiscard]] sg_sampler iblCubeSampler() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
