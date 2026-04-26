#pragma once

#include <glm/glm.hpp>
#include <memory>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

struct Camera;

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

    /// Upload all mesh assets in `scene` to GPU buffers and pre-flatten the
    /// node hierarchy into draw groups. Discards previous scene state. Call
    /// after sg_setup.
    void upload(const RenderScene &scene);

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
        bool enable_pbr{false};
        bool enable_ibl{false};
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
    [[nodiscard]] FrameStats last_frame_stats() const;

    /// World-space AABB of the loaded scene. Returns false if no scene is loaded.
    [[nodiscard]] bool world_bounds(glm::vec3 &min, glm::vec3 &max) const;

    [[nodiscard]] uint32_t mesh_asset_count() const;
    [[nodiscard]] uint32_t node_count() const;
    [[nodiscard]] uint64_t triangle_count() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::viewer
