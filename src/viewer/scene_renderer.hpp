#pragma once

#include <glm/glm.hpp>
#include <memory>

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

    /// Release every sokol_gfx handle the renderer holds. Must be called
    /// before sg_shutdown — sokol asserts on outstanding resources at
    /// shutdown. Idempotent.
    void release();

    struct RenderFlags {
        bool wireframe{false};
        bool cull_back{false};
        bool angle_cut{false};
        bool shader_angle_cut{true};
        float angle_cut_start_deg{0.f};
        float angle_cut_end_deg{90.f};
        bool enable_pbr{false}; ///< When on: Cook-Torrance + IBL ambient.
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

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
