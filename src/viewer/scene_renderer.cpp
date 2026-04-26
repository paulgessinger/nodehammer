#include <nodehammer/viewer/scene_renderer.hpp>

#include <nodehammer/viewer/camera.hpp>

#include <ankerl/unordered_dense.h>
#include <glm/gtc/type_ptr.hpp>
#include <nodehammer/ir/render.hpp>
#include <sokol_gfx.h>

#include "scene.glsl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace nodehammer::viewer {

namespace {

struct GpuMesh {
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    uint32_t index_count{0};
    uint64_t triangle_count{0};
    glm::vec3 local_min{0.f};
    glm::vec3 local_max{0.f};
};

/// Transform an AABB by a 4x4 affine matrix using Arvo's trick — cheaper
/// than transforming all 8 corners. Each output axis is built from the
/// positive/negative contributions of each row of the matrix.
inline void transform_aabb(const glm::mat4 &m, const glm::vec3 &lmin, const glm::vec3 &lmax,
                           glm::vec3 &wmin, glm::vec3 &wmax) {
    glm::vec3 tr = glm::vec3(m[3]);
    wmin = tr;
    wmax = tr;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const float a = m[j][i] * lmin[j];
            const float b = m[j][i] * lmax[j];
            wmin[i] += std::min(a, b);
            wmax[i] += std::max(a, b);
        }
    }
}

struct InstanceGroupKey {
    MeshAssetId mesh;
    RenderMaterialId material;
    bool operator==(const InstanceGroupKey &o) const noexcept {
        return mesh == o.mesh && material == o.material;
    }
};

struct InstanceGroupKeyHash {
    using is_avalanching = void;
    [[nodiscard]] uint64_t operator()(const InstanceGroupKey &k) const noexcept {
        return ankerl::unordered_dense::detail::wyhash::mix(k.mesh.value, k.material.value);
    }
};

constexpr int kInstanceVbufSlot = 1;

} // namespace

struct SceneRenderer::Impl {
    bool initialised{false};
    sg_shader shader{};
    sg_pipeline pipeline_no_cull{};
    sg_pipeline pipeline_back_cull{};
    // Per-group dynamic buffer of mat4 instance transforms. Allocated once
    // at upload() to fit the largest group; reused across frames since the
    // scene is static. Holds GROUPS-worth of data at start of each render().
    sg_buffer instance_buf{};
    size_t instance_buf_capacity{0}; // bytes

    ankerl::unordered_dense::map<MeshAssetId, GpuMesh> meshes;
    ankerl::unordered_dense::map<RenderMaterialId, glm::vec4> material_colors;

    struct DrawGroup {
        MeshAssetId mesh;
        RenderMaterialId material;
        size_t instance_byte_offset{0}; // where in instance_buf this group's matrices live
        std::vector<glm::mat4> instances;
    };
    std::vector<DrawGroup> groups;

    uint32_t node_count{0};
    uint64_t triangle_count{0};
    glm::vec3 bounds_min{0.f};
    glm::vec3 bounds_max{0.f};
    bool has_bounds{false};

    FrameStats last_stats;

    void ensure_init();
    void destroy_gpu();
    void upload_instance_buffer();
};

void SceneRenderer::Impl::ensure_init() {
    if (initialised) {
        return;
    }
    shader = sg_make_shader(scene_scene_shader_desc(sg_query_backend()));

    sg_pipeline_desc pdesc{};
    pdesc.shader = shader;
    pdesc.layout.buffers[0].stride = static_cast<int>(sizeof(Vertex));
    pdesc.layout.buffers[1].stride = static_cast<int>(sizeof(glm::mat4));
    pdesc.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;

    // Vertex attributes (slot indices come from sokol-shdc's reflection so
    // we don't hard-code the GLSL `layout(location=…)` numbers — sokol-shdc
    // assigns them in declaration order but tying via name keeps the C++
    // and shader sources independently editable).
    pdesc.layout.attrs[ATTR_scene_scene_a_position].buffer_index = 0;
    pdesc.layout.attrs[ATTR_scene_scene_a_position].format = SG_VERTEXFORMAT_FLOAT3;
    pdesc.layout.attrs[ATTR_scene_scene_a_position].offset = offsetof(Vertex, position);

    pdesc.layout.attrs[ATTR_scene_scene_a_normal].buffer_index = 0;
    pdesc.layout.attrs[ATTR_scene_scene_a_normal].format = SG_VERTEXFORMAT_FLOAT3;
    pdesc.layout.attrs[ATTR_scene_scene_a_normal].offset = offsetof(Vertex, normal);

    // Per-instance world matrix as 4 vec4 attributes packed into the second
    // vertex buffer.
    const int inst_attrs[4] = {
        ATTR_scene_scene_inst0,
        ATTR_scene_scene_inst1,
        ATTR_scene_scene_inst2,
        ATTR_scene_scene_inst3,
    };
    for (int i = 0; i < 4; ++i) {
        pdesc.layout.attrs[inst_attrs[i]].buffer_index = kInstanceVbufSlot;
        pdesc.layout.attrs[inst_attrs[i]].format = SG_VERTEXFORMAT_FLOAT4;
        pdesc.layout.attrs[inst_attrs[i]].offset =
            static_cast<int>(static_cast<size_t>(i) * sizeof(glm::vec4));
    }

    pdesc.index_type = SG_INDEXTYPE_UINT32;
    pdesc.depth.write_enabled = true;
    // Reversed-Z: near maps to 1, far maps to 0, so closer fragments have
    // LARGER depth values. Pair with depth-clear=0 in the pass action.
    pdesc.depth.compare = SG_COMPAREFUNC_GREATER_EQUAL;
    pdesc.face_winding = SG_FACEWINDING_CCW;
    pdesc.cull_mode = SG_CULLMODE_NONE;
    pipeline_no_cull = sg_make_pipeline(&pdesc);
    pdesc.cull_mode = SG_CULLMODE_BACK;
    pipeline_back_cull = sg_make_pipeline(&pdesc);

    initialised = true;
}

void SceneRenderer::Impl::destroy_gpu() {
    for (auto &[id, m] : meshes) {
        if (m.vbuf.id != SG_INVALID_ID) {
            sg_destroy_buffer(m.vbuf);
        }
        if (m.ibuf.id != SG_INVALID_ID) {
            sg_destroy_buffer(m.ibuf);
        }
    }
    meshes.clear();
    material_colors.clear();
    groups.clear();
    if (instance_buf.id != SG_INVALID_ID) {
        sg_destroy_buffer(instance_buf);
        instance_buf = sg_buffer{};
    }
    instance_buf_capacity = 0;
    node_count = 0;
    triangle_count = 0;
    has_bounds = false;
}

void SceneRenderer::Impl::upload_instance_buffer() {
    // Total mat4 count across all groups; pack contiguously.
    size_t total = 0;
    for (auto &g : groups) {
        g.instance_byte_offset = total * sizeof(glm::mat4);
        total += g.instances.size();
    }
    if (total == 0) {
        return;
    }
    std::vector<glm::mat4> packed;
    packed.reserve(total);
    for (const auto &g : groups) {
        packed.insert(packed.end(), g.instances.begin(), g.instances.end());
    }
    const size_t bytes = packed.size() * sizeof(glm::mat4);

    sg_buffer_desc bdesc{};
    bdesc.size = bytes;
    bdesc.usage.vertex_buffer = true;
    bdesc.usage.immutable = true;
    bdesc.data.ptr = packed.data();
    bdesc.data.size = bytes;
    instance_buf = sg_make_buffer(&bdesc);
    instance_buf_capacity = bytes;
}

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}

SceneRenderer::~SceneRenderer() {
    // Intentionally empty. sokol_gfx asserts at sg_shutdown if any handle
    // outlives it; the App lifecycle calls release() in the right order.
}

void SceneRenderer::release() {
    if (!impl_ || !impl_->initialised) {
        return;
    }
    impl_->destroy_gpu();
    if (impl_->pipeline_no_cull.id != SG_INVALID_ID) {
        sg_destroy_pipeline(impl_->pipeline_no_cull);
        impl_->pipeline_no_cull = sg_pipeline{};
    }
    if (impl_->pipeline_back_cull.id != SG_INVALID_ID) {
        sg_destroy_pipeline(impl_->pipeline_back_cull);
        impl_->pipeline_back_cull = sg_pipeline{};
    }
    if (impl_->shader.id != SG_INVALID_ID) {
        sg_destroy_shader(impl_->shader);
        impl_->shader = sg_shader{};
    }
    impl_->initialised = false;
}

void SceneRenderer::upload(const RenderScene &scene) {
    impl_->ensure_init();
    impl_->destroy_gpu();

    static_assert(sizeof(Vertex) == 24, "Vertex layout must match shader: 3f position + 3f normal");

    for (const auto &[id, asset] : scene.meshAssets) {
        if (asset.vertices.empty() || asset.indices.empty()) {
            continue;
        }
        GpuMesh gm;

        sg_buffer_desc vdesc{};
        vdesc.size = asset.vertices.size() * sizeof(Vertex);
        vdesc.usage.vertex_buffer = true;
        vdesc.usage.immutable = true;
        vdesc.data.ptr = asset.vertices.data();
        vdesc.data.size = vdesc.size;
        gm.vbuf = sg_make_buffer(&vdesc);

        sg_buffer_desc idesc{};
        idesc.size = asset.indices.size() * sizeof(uint32_t);
        idesc.usage.index_buffer = true;
        idesc.usage.immutable = true;
        idesc.data.ptr = asset.indices.data();
        idesc.data.size = idesc.size;
        gm.ibuf = sg_make_buffer(&idesc);

        gm.index_count = static_cast<uint32_t>(asset.indices.size());
        gm.triangle_count = asset.indices.size() / 3;

        glm::vec3 lmin{std::numeric_limits<float>::max()};
        glm::vec3 lmax{std::numeric_limits<float>::lowest()};
        for (const auto &v : asset.vertices) {
            lmin = glm::min(lmin, v.position);
            lmax = glm::max(lmax, v.position);
        }
        gm.local_min = lmin;
        gm.local_max = lmax;
        impl_->meshes.emplace(id, gm);
    }

    for (const auto &[id, mat] : scene.materials) {
        impl_->material_colors.emplace(id, mat.baseColorFactor);
    }

    ankerl::unordered_dense::map<InstanceGroupKey, size_t, InstanceGroupKeyHash> group_idx;
    glm::vec3 bmin{std::numeric_limits<float>::max()};
    glm::vec3 bmax{std::numeric_limits<float>::lowest()};
    bool any_bounds = false;

    for (const auto &[id, node] : scene.nodes) {
        for (const auto &binding : node.meshBindings) {
            auto mesh_it = impl_->meshes.find(binding.meshId);
            if (mesh_it == impl_->meshes.end()) {
                continue;
            }
            const InstanceGroupKey key{binding.meshId, binding.materialId};
            auto [it, inserted] = group_idx.try_emplace(key, impl_->groups.size());
            if (inserted) {
                impl_->groups.push_back({binding.meshId, binding.materialId, 0, {}});
            }
            impl_->groups[it->second].instances.push_back(node.worldTransform);
            ++impl_->node_count;
            impl_->triangle_count += mesh_it->second.triangle_count;

            glm::vec3 wmin, wmax;
            transform_aabb(node.worldTransform, mesh_it->second.local_min,
                           mesh_it->second.local_max, wmin, wmax);
            bmin = glm::min(bmin, wmin);
            bmax = glm::max(bmax, wmax);
            any_bounds = true;
        }
    }
    impl_->bounds_min = bmin;
    impl_->bounds_max = bmax;
    impl_->has_bounds = any_bounds;

    impl_->upload_instance_buffer();

    std::fprintf(stderr,
                 "viewer: scene uploaded — meshes=%zu materials=%zu nodes=%u groups=%zu "
                 "tris=%llu bbox=[%.1f %.1f %.1f .. %.1f %.1f %.1f]\n",
                 impl_->meshes.size(), impl_->material_colors.size(), impl_->node_count,
                 impl_->groups.size(), static_cast<unsigned long long>(impl_->triangle_count),
                 bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);
}

void SceneRenderer::render(const Camera &camera, uint32_t fb_width, uint32_t fb_height,
                           RenderFlags flags) {
    impl_->last_stats = {};
    const sg_pipeline pipeline =
        flags.cull_back ? impl_->pipeline_back_cull : impl_->pipeline_no_cull;
    if (impl_->groups.empty() || pipeline.id == SG_INVALID_ID) {
        return;
    }
    if (fb_width == 0 || fb_height == 0) {
        return;
    }

    sg_apply_pipeline(pipeline);

    (void)flags.wireframe; // wireframe also pipeline-state; same future work.

    // sokol_gfx is depth-range-agnostic at the API level — view_proj must
    // be built for the active backend's clip-space convention. Pass the
    // homogeneous-depth flag to camera::proj based on the backend so GL/GLES
    // get [-1,1] z and Metal/D3D/WebGPU get [0,1] z.
    const sg_backend backend = sg_query_backend();
    const bool homogeneous_depth = (backend == SG_BACKEND_GLCORE) || (backend == SG_BACKEND_GLES3);
    const float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);
    const glm::mat4 view = camera.view();
    // Reversed-Z projection — must match the pipeline's GREATER_EQUAL compare
    // and the pass action's depth clear of 0.0 in app.cpp.
    const glm::mat4 proj = camera.proj(aspect, homogeneous_depth, /*reversed_z=*/true);
    const glm::mat4 view_proj = proj * view;

    scene_vs_params_t vs_params{};
    std::memcpy(vs_params.view_proj, glm::value_ptr(view_proj), sizeof(vs_params.view_proj));
    sg_apply_uniforms(UB_scene_vs_params, SG_RANGE(vs_params));

    const glm::vec4 light_dir{-0.4f, -0.7f, -0.6f, 0.f};

    for (const auto &g : impl_->groups) {
        auto mesh_it = impl_->meshes.find(g.mesh);
        if (mesh_it == impl_->meshes.end()) {
            continue;
        }
        const auto &mesh = mesh_it->second;
        const auto inst_count = static_cast<int>(g.instances.size());
        if (inst_count == 0) {
            continue;
        }

        auto col_it = impl_->material_colors.find(g.material);
        const glm::vec4 base = (col_it != impl_->material_colors.end())
                                   ? col_it->second
                                   : glm::vec4{0.8f, 0.8f, 0.8f, 1.f};

        scene_fs_params_t fs_params{};
        std::memcpy(fs_params.base_color, glm::value_ptr(base), sizeof(fs_params.base_color));
        std::memcpy(fs_params.light_dir, glm::value_ptr(light_dir), sizeof(fs_params.light_dir));
        const glm::vec4 cut_params{flags.angle_cut ? 1.f : 0.f,
                                   glm::radians(flags.angle_cut_start_deg),
                                   glm::radians(flags.angle_cut_end_deg), 0.f};
        std::memcpy(fs_params.cut_params, glm::value_ptr(cut_params), sizeof(fs_params.cut_params));
        sg_apply_uniforms(UB_scene_fs_params, SG_RANGE(fs_params));

        sg_bindings bind{};
        bind.vertex_buffers[0] = mesh.vbuf;
        bind.vertex_buffers[1] = impl_->instance_buf;
        bind.vertex_buffer_offsets[1] = static_cast<int>(g.instance_byte_offset);
        bind.index_buffer = mesh.ibuf;
        sg_apply_bindings(&bind);

        sg_draw(0, static_cast<int>(mesh.index_count), inst_count);

        ++impl_->last_stats.draw_calls;
        impl_->last_stats.instances += static_cast<uint32_t>(inst_count);
        impl_->last_stats.triangles += mesh.triangle_count * static_cast<uint64_t>(inst_count);
    }
}

SceneRenderer::FrameStats SceneRenderer::last_frame_stats() const { return impl_->last_stats; }

bool SceneRenderer::world_bounds(glm::vec3 &min, glm::vec3 &max) const {
    if (!impl_->has_bounds) {
        return false;
    }
    min = impl_->bounds_min;
    max = impl_->bounds_max;
    return true;
}

uint32_t SceneRenderer::mesh_asset_count() const {
    return static_cast<uint32_t>(impl_->meshes.size());
}
uint32_t SceneRenderer::node_count() const { return impl_->node_count; }
uint64_t SceneRenderer::triangle_count() const { return impl_->triangle_count; }

} // namespace nodehammer::viewer
