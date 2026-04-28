#include "scene_renderer.hpp"

#include <nodehammer/viewer/camera.hpp>

#include <ankerl/unordered_dense.h>
#include <glm/gtc/type_ptr.hpp>
#include <nodehammer/ir/render.hpp>
#include <sokol_gfx.h>

#include "ibl.hpp"
#include "scene.glsl.h"

#include <sokol_time.h>

#include <algorithm>
#include <array>
#include <cmath>
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
inline void transformAabb(const glm::mat4 &m, const glm::vec3 &lmin, const glm::vec3 &lmax,
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
constexpr float kTau = 6.28318530717958647692f;

float normalizeAngle(float angle) {
    angle = std::fmod(angle, kTau);
    if (angle < 0.f) {
        angle += kTau;
    }
    return angle;
}

float angleSpan(float start, float end) {
    const float span = normalizeAngle(end) - normalizeAngle(start);
    return span >= 0.f ? span : span + kTau;
}

bool angleInRange(float angle, float start, float end) {
    angle = normalizeAngle(angle);
    start = normalizeAngle(start);
    end = normalizeAngle(end);
    if (std::abs(start - end) < 0.0001f) {
        return false;
    }
    if (start <= end) {
        return angle >= start && angle <= end;
    }
    return angle >= start || angle <= end;
}

bool aabbFullyInsideAngleCut(const glm::vec3 &min, const glm::vec3 &max, float start, float end) {
    if (std::abs(normalizeAngle(start) - normalizeAngle(end)) < 0.0001f) {
        return false;
    }
    if (min.x <= 0.f && max.x >= 0.f && min.y <= 0.f && max.y >= 0.f) {
        return false;
    }

    const float cut_width = angleSpan(start, end);
    const bool test_kept_sector = cut_width > 0.5f * kTau;
    const float test_start = test_kept_sector ? end : start;
    const float test_end = test_kept_sector ? start : end;

    const glm::vec2 corners[4] = {
        {min.x, min.y},
        {min.x, max.y},
        {max.x, min.y},
        {max.x, max.y},
    };
    for (const auto &corner : corners) {
        const bool in_test_sector =
            angleInRange(std::atan2(corner.y, corner.x), test_start, test_end);
        if (test_kept_sector) {
            if (in_test_sector) {
                return false;
            }
        } else if (!in_test_sector) {
            return false;
        }
    }
    return true;
}

std::array<glm::vec4, 6> frustumPlanes(const glm::mat4 &m) {
    const glm::vec4 row0{m[0][0], m[1][0], m[2][0], m[3][0]};
    const glm::vec4 row1{m[0][1], m[1][1], m[2][1], m[3][1]};
    const glm::vec4 row2{m[0][2], m[1][2], m[2][2], m[3][2]};
    const glm::vec4 row3{m[0][3], m[1][3], m[2][3], m[3][3]};

    std::array<glm::vec4, 6> planes{
        row3 + row0, // left
        row3 - row0, // right
        row3 + row1, // bottom
        row3 - row1, // top
        row3 + row2, // near
        row3 - row2, // far
    };
    for (auto &plane : planes) {
        const float len = glm::length(glm::vec3{plane});
        if (len > 0.f) {
            plane /= len;
        }
    }
    return planes;
}

bool aabbOutsideFrustum(const glm::vec3 &min, const glm::vec3 &max,
                        const std::array<glm::vec4, 6> &planes) {
    for (const auto &plane : planes) {
        const glm::vec3 positive{
            plane.x >= 0.f ? max.x : min.x,
            plane.y >= 0.f ? max.y : min.y,
            plane.z >= 0.f ? max.z : min.z,
        };
        if (glm::dot(glm::vec3{plane}, positive) + plane.w < 0.f) {
            return true;
        }
    }
    return false;
}

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
    struct GpuMaterial {
        glm::vec4 base_color{0.8f, 0.8f, 0.8f, 1.f};
        glm::vec4 mr{0.f, 0.5f, 0.f, 0.f}; // x=metallic, y=roughness
    };
    ankerl::unordered_dense::map<RenderMaterialId, GpuMaterial> materials;

    IblResources ibl;

    struct DrawGroup {
        MeshAssetId mesh;
        RenderMaterialId material;
        size_t visible_byte_offset{0}; // where in instance_buf this frame's matrices live
        uint32_t visible_count{0};
        std::vector<glm::mat4> instances;
        std::vector<glm::vec3> bounds_min;
        std::vector<glm::vec3> bounds_max;
    };
    std::vector<DrawGroup> groups;
    std::vector<glm::mat4> visible_instances;

    uint32_t node_count{0};
    uint64_t triangle_count{0};
    glm::vec3 bounds_min{0.f};
    glm::vec3 bounds_max{0.f};
    bool has_bounds{false};

    FrameStats last_stats;

    // Chunked-upload state. `pending_scene` keeps the source data alive
    // while we walk `pending_mesh_ids` across multiple frames. Once
    // `next_pending_mesh == pending_mesh_ids.size()`, the finalize step
    // (materials, groups, bounds, instance buffer) runs in one shot and
    // the upload is marked complete.
    std::shared_ptr<const RenderScene> pending_scene;
    std::vector<MeshAssetId> pending_mesh_ids;
    size_t next_pending_mesh{0};
    bool upload_busy{false};

    void ensureInit();
    void destroyGpu();
    void uploadInstanceBuffer();
    void uploadOneMesh(MeshAssetId id, const MeshAsset &asset);
    void finalizeUpload();
};

void SceneRenderer::Impl::ensureInit() {
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

    // IBL bindings start as 1×1 placeholder textures so the shader always
    // has something to sample from. The App owns the real bake and calls
    // `installIbl` to swap them in once it finishes.
    ibl.createDummy();

    initialised = true;
}

void SceneRenderer::Impl::destroyGpu() {
    for (auto &[id, m] : meshes) {
        if (m.vbuf.id != SG_INVALID_ID) {
            sg_destroy_buffer(m.vbuf);
        }
        if (m.ibuf.id != SG_INVALID_ID) {
            sg_destroy_buffer(m.ibuf);
        }
    }
    meshes.clear();
    materials.clear();
    groups.clear();
    visible_instances.clear();
    if (instance_buf.id != SG_INVALID_ID) {
        sg_destroy_buffer(instance_buf);
        instance_buf = sg_buffer{};
    }
    instance_buf_capacity = 0;
    node_count = 0;
    triangle_count = 0;
    has_bounds = false;
    // Drop any in-flight upload state — a fresh beginUpload is the only
    // way back to a populated scene.
    pending_scene.reset();
    pending_mesh_ids.clear();
    next_pending_mesh = 0;
    upload_busy = false;
}

void SceneRenderer::Impl::uploadInstanceBuffer() {
    size_t total = 0;
    for (const auto &g : groups) {
        total += g.instances.size();
    }
    if (total == 0) {
        return;
    }
    visible_instances.reserve(total);
    const size_t bytes = total * sizeof(glm::mat4);

    sg_buffer_desc bdesc{};
    bdesc.size = bytes;
    bdesc.usage.vertex_buffer = true;
    bdesc.usage.stream_update = true;
    instance_buf = sg_make_buffer(&bdesc);
    instance_buf_capacity = bytes;
}

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}

SceneRenderer::~SceneRenderer() {
    // Intentionally empty. sokol_gfx asserts at sg_shutdown if any handle
    // outlives it; the App lifecycle calls release() in the right order.
}

void SceneRenderer::initialize() { impl_->ensureInit(); }

void SceneRenderer::installIbl(const IblBakeData &data) {
    impl_->ensureInit();
    impl_->ibl.release();
    impl_->ibl.upload(data);
}

void SceneRenderer::release() {
    if (!impl_ || !impl_->initialised) {
        return;
    }
    impl_->destroyGpu();
    if (impl_->pipeline_no_cull.id != SG_INVALID_ID) {
        sg_destroy_pipeline(impl_->pipeline_no_cull);
        impl_->pipeline_no_cull = sg_pipeline{};
    }
    if (impl_->pipeline_back_cull.id != SG_INVALID_ID) {
        sg_destroy_pipeline(impl_->pipeline_back_cull);
        impl_->pipeline_back_cull = sg_pipeline{};
    }
    impl_->ibl.release();
    if (impl_->shader.id != SG_INVALID_ID) {
        sg_destroy_shader(impl_->shader);
        impl_->shader = sg_shader{};
    }
    impl_->initialised = false;
}

void SceneRenderer::Impl::uploadOneMesh(MeshAssetId id, const MeshAsset &asset) {
    if (asset.vertices.empty() || asset.indices.empty()) {
        return;
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
    meshes.emplace(id, gm);
}

void SceneRenderer::Impl::finalizeUpload() {
    const RenderScene &scene = *pending_scene;

    for (const auto &[id, mat] : scene.materials) {
        GpuMaterial gm;
        gm.base_color = mat.baseColorFactor;
        gm.mr = glm::vec4{mat.metallicFactor, mat.roughnessFactor, 0.f, 0.f};
        materials.emplace(id, gm);
    }

    ankerl::unordered_dense::map<InstanceGroupKey, size_t, InstanceGroupKeyHash> group_idx;
    glm::vec3 bmin{std::numeric_limits<float>::max()};
    glm::vec3 bmax{std::numeric_limits<float>::lowest()};
    bool any_bounds = false;

    for (const auto &[id, node] : scene.nodes) {
        for (const auto &binding : node.meshBindings) {
            auto mesh_it = meshes.find(binding.meshId);
            if (mesh_it == meshes.end()) {
                continue;
            }
            const InstanceGroupKey key{binding.meshId, binding.materialId};
            auto [it, inserted] = group_idx.try_emplace(key, groups.size());
            if (inserted) {
                groups.push_back({binding.meshId, binding.materialId, 0, 0, {}, {}, {}});
            }
            ++node_count;
            triangle_count += mesh_it->second.triangle_count;

            glm::vec3 wmin, wmax;
            transformAabb(node.worldTransform, mesh_it->second.local_min, mesh_it->second.local_max,
                          wmin, wmax);
            auto &group = groups[it->second];
            group.instances.push_back(node.worldTransform);
            group.bounds_min.push_back(wmin);
            group.bounds_max.push_back(wmax);
            bmin = glm::min(bmin, wmin);
            bmax = glm::max(bmax, wmax);
            any_bounds = true;
        }
    }
    bounds_min = bmin;
    bounds_max = bmax;
    has_bounds = any_bounds;

    uploadInstanceBuffer();

    std::println("viewer: scene uploaded — meshes={} materials={} nodes={} groups={} tris={} "
                 "bbox=[{:.1f} {:.1f} {:.1f} .. {:.1f} {:.1f} {:.1f}]\n",
                 meshes.size(), materials.size(), node_count, groups.size(),
                 static_cast<unsigned long long>(triangle_count), bmin.x, bmin.y, bmin.z, bmax.x,
                 bmax.y, bmax.z);

    pending_scene.reset();
    pending_mesh_ids.clear();
    pending_mesh_ids.shrink_to_fit();
    next_pending_mesh = 0;
    upload_busy = false;
}

void SceneRenderer::beginUpload(std::shared_ptr<const RenderScene> scene) {
    static_assert(sizeof(Vertex) == 24, "Vertex layout must match shader: 3f position + 3f normal");
    impl_->ensureInit();
    impl_->destroyGpu();

    impl_->pending_scene = std::move(scene);
    impl_->pending_mesh_ids.clear();
    if (impl_->pending_scene) {
        impl_->pending_mesh_ids.reserve(impl_->pending_scene->meshAssets.size());
        for (const auto &[id, _asset] : impl_->pending_scene->meshAssets) {
            impl_->pending_mesh_ids.push_back(id);
        }
    }
    impl_->next_pending_mesh = 0;
    impl_->upload_busy = impl_->pending_scene != nullptr;
}

bool SceneRenderer::advanceUpload(uint64_t budget_ns) {
    if (!impl_->upload_busy) {
        return true;
    }
    const uint64_t start_ticks = stm_now();
    while (impl_->next_pending_mesh < impl_->pending_mesh_ids.size()) {
        const auto id = impl_->pending_mesh_ids[impl_->next_pending_mesh++];
        const auto it = impl_->pending_scene->meshAssets.find(id);
        if (it != impl_->pending_scene->meshAssets.end()) {
            impl_->uploadOneMesh(id, it->second);
        }
        if (stm_ns(stm_diff(stm_now(), start_ticks)) >= static_cast<double>(budget_ns)) {
            // Yield to the next frame; meshes left in the queue resume on
            // the next advance.
            return false;
        }
    }
    impl_->finalizeUpload();
    return true;
}

bool SceneRenderer::uploadInProgress() const { return impl_->upload_busy; }

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
    const auto planes = frustumPlanes(view_proj);

    impl_->visible_instances.clear();
    const float cut_start = glm::radians(flags.angle_cut_start_deg);
    const float cut_end = glm::radians(flags.angle_cut_end_deg);
    for (auto &g : impl_->groups) {
        g.visible_byte_offset = impl_->visible_instances.size() * sizeof(glm::mat4);
        const size_t start_count = impl_->visible_instances.size();
        for (size_t i = 0; i < g.instances.size(); ++i) {
            if (aabbOutsideFrustum(g.bounds_min[i], g.bounds_max[i], planes)) {
                continue;
            }
            if (flags.angle_cut &&
                aabbFullyInsideAngleCut(g.bounds_min[i], g.bounds_max[i], cut_start, cut_end)) {
                continue;
            }
            impl_->visible_instances.push_back(g.instances[i]);
        }
        g.visible_count = static_cast<uint32_t>(impl_->visible_instances.size() - start_count);
    }
    if (impl_->visible_instances.empty()) {
        return;
    }
    const size_t visible_bytes = impl_->visible_instances.size() * sizeof(glm::mat4);
    sg_update_buffer(impl_->instance_buf, {impl_->visible_instances.data(), visible_bytes});

    scene_vs_params_t vs_params{};
    std::memcpy(vs_params.view_proj, glm::value_ptr(view_proj), sizeof(vs_params.view_proj));
    sg_apply_uniforms(UB_scene_vs_params, SG_RANGE(vs_params));

    // w = directional light intensity (used by PBR branch only; Lambert ignores).
    const glm::vec4 light_dir{-0.4f, -0.7f, -0.6f, 3.f};

    for (const auto &g : impl_->groups) {
        auto mesh_it = impl_->meshes.find(g.mesh);
        if (mesh_it == impl_->meshes.end()) {
            continue;
        }
        const auto &mesh = mesh_it->second;
        const auto inst_count = static_cast<int>(g.visible_count);
        if (inst_count == 0) {
            continue;
        }

        auto mat_it = impl_->materials.find(g.material);
        const Impl::GpuMaterial gmat =
            (mat_it != impl_->materials.end()) ? mat_it->second : Impl::GpuMaterial{};

        scene_fs_params_t fs_params{};
        std::memcpy(fs_params.base_color, glm::value_ptr(gmat.base_color),
                    sizeof(fs_params.base_color));
        std::memcpy(fs_params.light_dir, glm::value_ptr(light_dir), sizeof(fs_params.light_dir));
        const float shader_cut_start = glm::radians(flags.angle_cut_start_deg);
        const float shader_cut_end = glm::radians(flags.angle_cut_end_deg);
        const bool large_cut = angleSpan(shader_cut_start, shader_cut_end) > 0.5f * kTau;
        const glm::vec4 cut_params{(flags.angle_cut && flags.shader_angle_cut) ? 1.f : 0.f,
                                   large_cut ? 1.f : 0.f, 0.f, 0.f};
        const glm::vec4 cut_start_vec{std::cos(shader_cut_start), std::sin(shader_cut_start), 0.f,
                                      0.f};
        const glm::vec4 cut_end_vec{std::cos(shader_cut_end), std::sin(shader_cut_end), 0.f, 0.f};
        std::memcpy(fs_params.cut_params, glm::value_ptr(cut_params), sizeof(fs_params.cut_params));
        std::memcpy(fs_params.cut_start, glm::value_ptr(cut_start_vec),
                    sizeof(fs_params.cut_start));
        std::memcpy(fs_params.cut_end, glm::value_ptr(cut_end_vec), sizeof(fs_params.cut_end));
        std::memcpy(fs_params.material_mr, glm::value_ptr(gmat.mr), sizeof(fs_params.material_mr));
        const float prefilter_max_lod =
            std::max(0.f, static_cast<float>(impl_->ibl.prefilter_mip_count - 1));
        const glm::vec4 mode_flags{flags.enable_pbr ? 1.f : 0.f, prefilter_max_lod, 0.f, 0.f};
        std::memcpy(fs_params.mode_flags, glm::value_ptr(mode_flags), sizeof(fs_params.mode_flags));
        const glm::vec3 eye = camera.eye();
        const glm::vec4 cam_pos{eye.x, eye.y, eye.z, 0.f};
        std::memcpy(fs_params.camera_pos, glm::value_ptr(cam_pos), sizeof(fs_params.camera_pos));
        sg_apply_uniforms(UB_scene_fs_params, SG_RANGE(fs_params));

        sg_bindings bind{};
        bind.vertex_buffers[0] = mesh.vbuf;
        bind.vertex_buffers[1] = impl_->instance_buf;
        bind.vertex_buffer_offsets[1] = static_cast<int>(g.visible_byte_offset);
        bind.index_buffer = mesh.ibuf;
        bind.views[VIEW_scene_tex_irradiance] = impl_->ibl.irradiance_view;
        bind.views[VIEW_scene_tex_prefilter] = impl_->ibl.prefilter_view;
        bind.views[VIEW_scene_tex_brdf_lut] = impl_->ibl.brdf_lut_view;
        bind.samplers[SMP_scene_smp_cube] = impl_->ibl.cube_sampler;
        bind.samplers[SMP_scene_smp_lut] = impl_->ibl.lut_sampler;
        sg_apply_bindings(&bind);

        sg_draw(0, static_cast<int>(mesh.index_count), inst_count);

        ++impl_->last_stats.draw_calls;
        impl_->last_stats.instances += static_cast<uint32_t>(inst_count);
        impl_->last_stats.triangles += mesh.triangle_count * static_cast<uint64_t>(inst_count);
    }
}

SceneRenderer::FrameStats SceneRenderer::lastFrameStats() const { return impl_->last_stats; }

bool SceneRenderer::worldBounds(glm::vec3 &min, glm::vec3 &max) const {
    if (!impl_->has_bounds) {
        return false;
    }
    min = impl_->bounds_min;
    max = impl_->bounds_max;
    return true;
}

uint32_t SceneRenderer::meshAssetCount() const {
    return static_cast<uint32_t>(impl_->meshes.size());
}
uint32_t SceneRenderer::nodeCount() const { return impl_->node_count; }
uint64_t SceneRenderer::triangleCount() const { return impl_->triangle_count; }

} // namespace nodehammer::viewer
