#pragma once

#include <glm/glm.hpp>
#include <memory>

namespace nodehammer {
struct RenderScene;
} // namespace nodehammer

namespace nodehammer::viewer {

struct Camera;

/// GPU-side renderer for nodehammer's tessellated RenderScene IR.
///
/// Stage 3 first pass: per-mesh-asset static buffers, draw nodes grouped by
/// (mesh, material) with instancing when the group is large enough and the
/// backend supports it. Lambertian shading; PBR comes later.
class SceneRenderer {
  public:
    SceneRenderer();
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer &) = delete;
    SceneRenderer &operator=(const SceneRenderer &) = delete;

    /// Upload all mesh assets in `scene` to GPU buffers and pre-flatten the
    /// node hierarchy into draw groups. Discards previous scene state. Call
    /// after bgfx::init.
    void upload(const RenderScene &scene);

    /// Release every bgfx handle the renderer holds. Must be called before
    /// bgfx::shutdown — the destructor would otherwise attempt to destroy
    /// handles against a dead context. Idempotent.
    void release();

    struct RenderFlags {
        bool wireframe{false};
        bool cull_back{false};
    };

    /// Submit draw calls for the active scene to the given view. Sets the view
    /// transform from the camera; assumes the view rect was already configured
    /// for the framebuffer size.
    void render(uint16_t view_id, const Camera &camera, uint32_t fb_width, uint32_t fb_height,
                RenderFlags flags);

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
