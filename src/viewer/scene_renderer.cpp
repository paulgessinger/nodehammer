#include "scene_renderer.hpp"

#include <viewer/backend_caps.hpp>
#include <viewer/camera.hpp>

#include <ankerl/unordered_dense.h>
#include <glm/gtc/type_ptr.hpp>
#include <ir/render.hpp>
#include <sokol_gfx.h>

#include "ibl.hpp"
#include "scene.glsl.h"
#include "scene_render_target.hpp"

#include <sokol_time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <print>
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
    // Material-stack prefilter hint (from MeshAsset::stackAverage). feature_size
    // == 0 means the mesh isn't a tagged sampling stack, so the scene shader
    // skips the blend. See scene.glsl's stack_prefilter.
    glm::vec3 stack_avg_color{0.f};
    float stack_feature_size{0.f};
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
    ir::render::MeshAssetId mesh;
    ir::render::MaterialId material;
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

// Per-instance vertex-buffer record: the world matrix plus a LOD cross-fade
// factor (lod.x in [0,1]; 0 = full detail, 1 = full hull proxy). The matrix
// maps to attributes inst0..inst3, the fade to inst_lod (see scene.glsl). The
// fade is recomputed per frame from projected screen size, so it lives only in
// the per-frame upload buffer, not in the static per-group instance list.
struct InstanceGpu {
    glm::mat4 transform;
    glm::vec4 lod{0.f};
};

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
    // Overdraw debug pipeline: same shader + vertex layout, but additive
    // blending, depth test/write off, and no cull, so every covered fragment
    // accumulates a count regardless of occlusion. Rebuilt with the others
    // when the target color format changes.
    sg_pipeline pipeline_overdraw{};
    sg_pixel_format current_color_format{SG_PIXELFORMAT_NONE};
    // Per-group dynamic buffer of mat4 instance transforms. Allocated once
    // at upload() to fit the largest group; reused across frames since the
    // scene is static. Holds GROUPS-worth of data at start of each render().
    sg_buffer instance_buf{};
    size_t instance_buf_capacity{0}; // bytes

    ankerl::unordered_dense::map<ir::render::MeshAssetId, GpuMesh> meshes;
    struct GpuMaterial {
        glm::vec4 base_color{0.8f, 0.8f, 0.8f, 1.f};
        glm::vec4 mr{0.f, 0.5f, 0.f, 0.f};           // x=metallic, y=roughness
        glm::vec4 emissive{0.f};                     // xyz = emissive factor
        glm::vec4 alpha_params{0.f, 0.5f, 0.f, 0.f}; // x = alpha mode (0=OPAQUE,1=MASK), y = cutoff
        bool double_sided{true};
    };
    ankerl::unordered_dense::map<ir::render::MaterialId, GpuMaterial> materials;

    IblResources ibl;

    // LOD role of a draw group. Normal groups always draw; a stack with an LOD
    // hull produces a Detail group (its slabs) and a Proxy group (the hull),
    // and exactly one of the two draws depending on the hull-LOD toggle.
    enum class LodRole { Normal, Detail, Proxy };

    struct DrawGroup {
        ir::render::MeshAssetId mesh;
        ir::render::MaterialId material;
        bool double_sided{true};
        size_t visible_byte_offset{0}; // where in instance_buf this frame's matrices live
        uint32_t visible_count{0};
        std::vector<glm::mat4> instances;
        std::vector<glm::vec3> bounds_min;
        std::vector<glm::vec3> bounds_max;
        // Mesh-space bounding sphere (constant per group). Drives the LOD size
        // metric: transforming this center by the instance matrix and scaling
        // this radius by the transform's scale gives a *rotation-invariant*
        // projected size. The world AABB above can't be used for that -- it
        // inflates with orientation, so azimuthally placed calo staves would
        // straddle the hull-switch band unevenly (some hull, some detail at the
        // same distance). The AABB stays for frustum culling only.
        glm::vec3 local_center{0.f};
        float local_radius{0.f};
        LodRole lod_role{LodRole::Normal};
    };
    std::vector<DrawGroup> groups;
    std::vector<InstanceGpu> visible_instances;

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
    std::shared_ptr<const ir::render::Scene> pending_scene;
    std::vector<ir::render::MeshAssetId> pending_mesh_ids;
    size_t next_pending_mesh{0};
    bool upload_busy{false};

    void ensureInit();
    void ensurePipelines(sg_pixel_format color_fmt);
    void destroyGpu();
    void uploadInstanceBuffer();
    void uploadOneMesh(ir::render::MeshAssetId id, const ir::render::MeshAsset &asset);
    void finalizeUpload();
};

void SceneRenderer::Impl::ensureInit() {
    if (initialised) {
        return;
    }
    shader = sg_make_shader(scene_scene_shader_desc(sg_query_backend()));

    // IBL bindings start as 1×1 placeholder textures so the shader always
    // has something to sample from. The App owns the real bake and calls
    // `installIbl` to swap them in once it finishes.
    ibl.createDummy();

    initialised = true;
    // Pipelines are deferred to the first `ensurePipelines` call from
    // App::ensureSceneTarget, which knows the offscreen target's color
    // format (LDR swapchain default vs. HDR RGBA16F).
}

void SceneRenderer::Impl::ensurePipelines(sg_pixel_format color_fmt) {
    if (pipeline_no_cull.id != SG_INVALID_ID && current_color_format == color_fmt) {
        return;
    }
    if (pipeline_no_cull.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_no_cull);
        pipeline_no_cull = sg_pipeline{};
    }
    if (pipeline_back_cull.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_back_cull);
        pipeline_back_cull = sg_pipeline{};
    }
    if (pipeline_overdraw.id != SG_INVALID_ID) {
        sg_destroy_pipeline(pipeline_overdraw);
        pipeline_overdraw = sg_pipeline{};
    }

    sg_pipeline_desc pdesc{};
    pdesc.shader = shader;
    pdesc.layout.buffers[0].stride = static_cast<int>(sizeof(ir::render::Vertex));
    pdesc.layout.buffers[1].stride = static_cast<int>(sizeof(InstanceGpu));
    pdesc.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;

    // Vertex attributes (slot indices come from sokol-shdc's reflection so
    // we don't hard-code the GLSL `layout(location=…)` numbers — sokol-shdc
    // assigns them in declaration order but tying via name keeps the C++
    // and shader sources independently editable).
    pdesc.layout.attrs[ATTR_scene_scene_a_position].buffer_index = 0;
    pdesc.layout.attrs[ATTR_scene_scene_a_position].format = SG_VERTEXFORMAT_FLOAT3;
    pdesc.layout.attrs[ATTR_scene_scene_a_position].offset = offsetof(ir::render::Vertex, position);

    pdesc.layout.attrs[ATTR_scene_scene_a_normal].buffer_index = 0;
    pdesc.layout.attrs[ATTR_scene_scene_a_normal].format = SG_VERTEXFORMAT_FLOAT3;
    pdesc.layout.attrs[ATTR_scene_scene_a_normal].offset = offsetof(ir::render::Vertex, normal);

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
        pdesc.layout.attrs[inst_attrs[i]].offset = static_cast<int>(
            offsetof(InstanceGpu, transform) + static_cast<size_t>(i) * sizeof(glm::vec4));
    }
    // Per-instance LOD cross-fade factor (see InstanceGpu / scene.glsl).
    pdesc.layout.attrs[ATTR_scene_scene_inst_lod].buffer_index = kInstanceVbufSlot;
    pdesc.layout.attrs[ATTR_scene_scene_inst_lod].format = SG_VERTEXFORMAT_FLOAT4;
    pdesc.layout.attrs[ATTR_scene_scene_inst_lod].offset =
        static_cast<int>(offsetof(InstanceGpu, lod));

    pdesc.index_type = SG_INDEXTYPE_UINT32;
    pdesc.depth.write_enabled = true;
    // Depth convention is backend-conditional (see useReversedZ): reversed-Z
    // (GREATER_EQUAL, near→1, depth-clear=0) on `[0,1]` clip-depth backends;
    // normal-Z (LESS_EQUAL, near→0, depth-clear=1) on GLES3 where reversed-Z
    // would degrade to normal-Z precision anyway and break Hi-Z. The
    // projection matrix in render() and the pass-action clear in app.cpp
    // must use the same flag.
    pdesc.depth.compare = useReversedZ() ? SG_COMPAREFUNC_GREATER_EQUAL : SG_COMPAREFUNC_LESS_EQUAL;
    pdesc.face_winding = SG_FACEWINDING_CCW;

    // Pin the offscreen scene-target color format. WebGPU rejects render
    // passes whose attachment format differs from the pipeline's declared
    // format, so toggling HDR (RGBA16F) on/off requires rebuilding both
    // pipelines.
    pdesc.color_count = 1;
    pdesc.colors[0].pixel_format = color_fmt;

    pdesc.cull_mode = SG_CULLMODE_NONE;
    pipeline_no_cull = sg_make_pipeline(&pdesc);
    pdesc.cull_mode = SG_CULLMODE_BACK;
    pipeline_back_cull = sg_make_pipeline(&pdesc);

    // Overdraw variant: count every covering fragment regardless of
    // occlusion or facing. Depth test/write off (ALWAYS), no cull, and
    // additive ONE/ONE blend so the FS's constant increment accumulates
    // into the color target. Reuses the shared layout/format above.
    pdesc.cull_mode = SG_CULLMODE_NONE;
    pdesc.depth.write_enabled = false;
    pdesc.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pdesc.colors[0].blend.enabled = true;
    pdesc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
    pdesc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
    pdesc.colors[0].blend.op_rgb = SG_BLENDOP_ADD;
    pdesc.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pdesc.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pdesc.colors[0].blend.op_alpha = SG_BLENDOP_ADD;
    pdesc.label = "scene_overdraw_pipeline";
    pipeline_overdraw = sg_make_pipeline(&pdesc);

    current_color_format = color_fmt;
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
    // Sized for every static instance across all groups. A stack in the LOD
    // cross-fade band draws to both its Detail and Proxy group in the same
    // frame, but those are separate static instances (addBindings pushed one to
    // each), so this sum already covers the worst case of the whole scene mid-
    // fade.
    visible_instances.reserve(total);
    const size_t bytes = total * sizeof(InstanceGpu);

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

void SceneRenderer::setTargetColorFormat(sg_pixel_format fmt) {
    impl_->ensureInit();
    impl_->ensurePipelines(fmt);
}

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
    if (impl_->pipeline_overdraw.id != SG_INVALID_ID) {
        sg_destroy_pipeline(impl_->pipeline_overdraw);
        impl_->pipeline_overdraw = sg_pipeline{};
    }
    impl_->current_color_format = SG_PIXELFORMAT_NONE;
    impl_->ibl.release();
    if (impl_->shader.id != SG_INVALID_ID) {
        sg_destroy_shader(impl_->shader);
        impl_->shader = sg_shader{};
    }
    impl_->initialised = false;
}

void SceneRenderer::Impl::uploadOneMesh(ir::render::MeshAssetId id,
                                        const ir::render::MeshAsset &asset) {
    if (asset.vertices.empty() || asset.indices.empty()) {
        return;
    }
    GpuMesh gm;

    sg_buffer_desc vdesc{};
    vdesc.size = asset.vertices.size() * sizeof(ir::render::Vertex);
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
    if (asset.stackAverage.has_value()) {
        gm.stack_avg_color = asset.stackAverage->avgColorLinear;
        gm.stack_feature_size = asset.stackAverage->featureSize;
    }
    meshes.emplace(id, gm);
}

void SceneRenderer::Impl::finalizeUpload() {
    const ir::render::Scene &scene = *pending_scene;

    for (const auto &[id, mat] : scene.materials) {
        GpuMaterial gm;
        gm.base_color = mat.baseColorFactor;
        gm.mr = glm::vec4{mat.metallicFactor, mat.roughnessFactor, 0.f, 0.f};
        gm.emissive = glm::vec4{mat.emissiveFactor, 0.f};
        const float alpha_mode = (mat.alphaMode == "MASK") ? 1.f : 0.f;
        gm.alpha_params = glm::vec4{alpha_mode, mat.alphaCutoff, 0.f, 0.f};
        gm.double_sided = mat.doubleSided;
        materials.emplace(id, gm);
    }

    ankerl::unordered_dense::map<InstanceGroupKey, size_t, InstanceGroupKeyHash> group_idx;
    glm::vec3 bmin{std::numeric_limits<float>::max()};
    glm::vec3 bmax{std::numeric_limits<float>::lowest()};
    bool any_bounds = false;

    auto addBindings = [&](const ir::render::Node &node,
                           const std::vector<ir::render::MeshBinding> &bindings, LodRole role) {
        for (const auto &binding : bindings) {
            auto mesh_it = meshes.find(binding.meshId);
            if (mesh_it == meshes.end()) {
                continue;
            }
            const InstanceGroupKey key{binding.meshId, binding.materialId};
            auto [it, inserted] = group_idx.try_emplace(key, groups.size());
            if (inserted) {
                auto mat_it = scene.materials.find(binding.materialId);
                // Missing material → single-sided (cull), matching the
                // render::Material default for closed solids.
                const bool double_sided =
                    mat_it != scene.materials.end() ? mat_it->second.doubleSided : false;
                DrawGroup g;
                g.mesh = binding.meshId;
                g.material = binding.materialId;
                g.double_sided = double_sided;
                g.lod_role = role;
                // Mesh-space bounding sphere, computed once from the local AABB.
                // Rotation-invariant (it lives in the mesh's own frame), so the
                // per-instance LOD size only picks up the transform's scale.
                g.local_center = 0.5f * (mesh_it->second.local_min + mesh_it->second.local_max);
                g.local_radius =
                    0.5f * glm::length(mesh_it->second.local_max - mesh_it->second.local_min);
                groups.push_back(std::move(g));
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
    };
    for (const auto &[id, node] : scene.nodes) {
        // A node with an LOD proxy tags its detailed groups as Detail (drawn
        // only when hull LOD is off); the proxy groups are Proxy (drawn only
        // when it's on). Nodes without a proxy are Normal (always drawn).
        const LodRole detailRole =
            node.lodProxyBindings.empty() ? LodRole::Normal : LodRole::Detail;
        addBindings(node, node.meshBindings, detailRole);
        addBindings(node, node.lodProxyBindings, LodRole::Proxy);
    }
    bounds_min = bmin;
    bounds_max = bmax;
    has_bounds = any_bounds;

    uploadInstanceBuffer();

    std::println("viewer: scene uploaded -- meshes={} materials={} nodes={} groups={} tris={} "
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

void SceneRenderer::beginUpload(std::shared_ptr<const ir::render::Scene> scene) {
    static_assert(sizeof(ir::render::Vertex) == 24,
                  "Vertex layout must match shader: 3f position + 3f normal");
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

void SceneRenderer::clearScene() {
    if (!impl_ || !impl_->initialised) {
        return;
    }
    impl_->destroyGpu();
}

void SceneRenderer::render(const Camera &camera, uint32_t fb_width, uint32_t fb_height,
                           RenderFlags flags) {
    impl_->last_stats = {};
    if (impl_->groups.empty() || impl_->pipeline_no_cull.id == SG_INVALID_ID ||
        impl_->pipeline_back_cull.id == SG_INVALID_ID) {
        return;
    }
    if (fb_width == 0 || fb_height == 0) {
        return;
    }

    (void)flags.wireframe; // wireframe also pipeline-state; same future work.

    // sokol_gfx is depth-range-agnostic at the API level — view_proj must
    // be built for the active backend's clip-space convention. Pass the
    // homogeneous-depth flag to camera::proj based on the backend so GL/GLES
    // get [-1,1] z and Metal/D3D/WebGPU get [0,1] z.
    const sg_backend backend = sg_query_backend();
    const bool homogeneous_depth = (backend == SG_BACKEND_GLCORE) || (backend == SG_BACKEND_GLES3);
    const float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);
    const glm::mat4 view = camera.view();
    // Projection convention must match the pipeline's depth.compare and the
    // pass action's depth.clear_value (see useReversedZ for why GLES3
    // diverges from the other backends).
    const bool reversed_z = useReversedZ();
    const glm::mat4 proj = camera.proj(aspect, homogeneous_depth, reversed_z);
    const glm::mat4 view_proj = proj * view;
    const auto planes = frustumPlanes(view_proj);

    impl_->visible_instances.clear();
    const float cut_start = glm::radians(flags.angle_cut_start_deg);
    const float cut_end = glm::radians(flags.angle_cut_end_deg);

    // Per-instance LOD cross-fade factor from projected screen size. `cam_right`
    // is the camera's world-space right axis (first row of the view rotation);
    // projecting the instance center and a point offset by its bounding radius
    // along that axis and measuring the clip-space delta yields a pixel radius
    // that is correct for both perspective and orthographic cameras (w = 1 for
    // ortho). Large on screen (close) -> 0 (draw detailed slabs); small (far) ->
    // 1 (draw hull proxy); the band in between draws both and dithers.
    //
    // The size comes from the mesh-space bounding *sphere* (per-group local
    // center/radius) transformed by the instance matrix -- NOT the world AABB.
    // A sphere is rotation-invariant, so two identical stacks at different
    // azimuths get the same projected size and switch together. Deriving the
    // size from the world AABB instead inflates it by up to ~sqrt(2) for slabs
    // rotated off the world axes, which splits a ring of calo staves across the
    // switch band (some hull, some detail at the same distance).
    const glm::vec3 cam_right{view[0][0], view[1][0], view[2][0]};
    const float lod_band = std::max(1.f, flags.lod_hull_band_px);
    const float lod_lo = std::max(0.f, flags.lod_hull_screen_px - lod_band);
    const float lod_hi = flags.lod_hull_screen_px + lod_band;
    const float fb_width_f = static_cast<float>(fb_width);
    auto lodFade = [&](const glm::mat4 &m, const glm::vec3 &local_center,
                       float local_radius) -> float {
        const glm::vec3 center = glm::vec3(m * glm::vec4{local_center, 1.f});
        // Largest axis scale of the instance transform; scales the mesh-space
        // radius into world space without re-AABB'ing the rotated box.
        const float scale = std::max({glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])),
                                      glm::length(glm::vec3(m[2]))});
        const float radius = local_radius * scale;
        const glm::vec4 c0 = view_proj * glm::vec4{center, 1.f};
        const glm::vec4 c1 = view_proj * glm::vec4{center + radius * cam_right, 1.f};
        const float w0 = std::max(1e-4f, std::abs(c0.w));
        const float w1 = std::max(1e-4f, std::abs(c1.w));
        const float screen_px = std::abs(c1.x / w1 - c0.x / w0) * 0.5f * fb_width_f;
        return 1.f - glm::smoothstep(lod_lo, lod_hi, screen_px);
    };

    for (auto &g : impl_->groups) {
        g.visible_byte_offset = impl_->visible_instances.size() * sizeof(InstanceGpu);

        const bool is_detail = g.lod_role == Impl::LodRole::Detail;
        const bool is_proxy = g.lod_role == Impl::LodRole::Proxy;
        // Whole-group skips: with per-distance LOD off, the Proxy side never
        // draws (detail everywhere); the force debug pins the opposite (hull
        // everywhere) and drops the Detail side. In between, both sides draw and
        // each instance decides per representation below.
        if (is_proxy && !flags.lod_hull_enable && !flags.lod_hull_force) {
            g.visible_count = 0;
            continue;
        }
        if (is_detail && flags.lod_hull_force) {
            g.visible_count = 0;
            continue;
        }

        const size_t start_count = impl_->visible_instances.size();
        for (size_t i = 0; i < g.instances.size(); ++i) {
            if (aabbOutsideFrustum(g.bounds_min[i], g.bounds_max[i], planes)) {
                continue;
            }
            if (flags.angle_cut &&
                aabbFullyInsideAngleCut(g.bounds_min[i], g.bounds_max[i], cut_start, cut_end)) {
                continue;
            }
            float fade = 0.f;
            if (is_detail || is_proxy) {
                if (flags.lod_hull_force) {
                    fade = 1.f;
                } else if (flags.lod_hull_enable) {
                    fade = lodFade(g.instances[i], g.local_center, g.local_radius);
                }
                // Skip instances the other representation fully owns: a fully
                // faded-in stack contributes nothing to Detail, a fully
                // faded-out one nothing to Proxy. In the band both keep it.
                if (is_detail && fade >= 1.f) {
                    continue;
                }
                if (is_proxy && fade <= 0.f) {
                    continue;
                }
            }
            impl_->visible_instances.push_back(
                InstanceGpu{g.instances[i], glm::vec4{fade, 0.f, 0.f, 0.f}});
        }
        g.visible_count = static_cast<uint32_t>(impl_->visible_instances.size() - start_count);
    }
    if (impl_->visible_instances.empty()) {
        return;
    }
    const size_t visible_bytes = impl_->visible_instances.size() * sizeof(InstanceGpu);
    sg_update_buffer(impl_->instance_buf, {impl_->visible_instances.data(), visible_bytes});

    scene_vs_params_t vs_params{};
    std::memcpy(vs_params.view_proj, glm::value_ptr(view_proj), sizeof(vs_params.view_proj));
    // Log depth (see useLogDepth) overrides gl_Position.z in the VS to give
    // near-uniform precision on backends where reversed-Z doesn't work
    // (GLES3 today). It depends on perspective clip W as a view-distance
    // proxy, so keep orthographic on the backend's normal depth path.
    const bool log_depth = useLogDepth() && camera.projection == ProjectionMode::Perspective;
    vs_params.depth_params[0] = log_depth ? 1.0f : 0.0f;
    vs_params.depth_params[1] = camera.far_plane;
    vs_params.depth_params[2] = 0.0f;
    vs_params.depth_params[3] = 0.0f;

    // Shader convention is "from-light direction" (light vector pointing toward
    // the surface); RenderFlags carries the toward-sun direction (matching the
    // IBL bake), so negate. w = directional light intensity (used by PBR branch
    // only; Lambert ignores).
    const glm::vec3 to_sun = glm::normalize(flags.sun_dir);
    const glm::vec4 light_dir{-to_sun, flags.sun_intensity};

    // Overdraw debug emits a constant per-fragment increment (through
    // mode_flags.z) that additive-blends into the color target. The value is
    // format-dependent so an 8-bit target doesn't clamp to 1.0 after the
    // first fragment (see overdrawColorIncrement). 0 outside overdraw mode
    // leaves the FS on its normal shading path.
    const float overdraw_increment =
        flags.overdraw ? overdrawColorIncrement(impl_->current_color_format) : 0.f;

    sg_pipeline current_pipeline{};
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

        // Overdraw debug overrides pipeline selection entirely: every group
        // draws through the additive / no-depth / no-cull overdraw pipeline
        // so the count reflects all covering fragments regardless of the
        // material's cull mode.
        sg_pipeline pipeline;
        if (flags.overdraw) {
            pipeline = impl_->pipeline_overdraw;
        } else {
            // Per-material doubleSided picks the cull pipeline by default. The
            // `cull` override is a debug knob that can force one mode globally.
            // Switch only when the pipeline actually changes; VS uniforms must
            // be reapplied after each pipeline change.
            bool use_back_cull = !g.double_sided;
            // The shader angle-cut discards the near wedge and exposes the interior
            // back-faces of the far wall; culling them makes the cut-away look
            // hollow. Render double-sided while it's active so cuts stay solid.
            // (The baked boolean cut adds watertight caps, so it keeps culling.)
            if (flags.shader_angle_cut) {
                use_back_cull = false;
            }
            if (flags.cull == CullOverride::ForceCull) {
                use_back_cull = true;
            } else if (flags.cull == CullOverride::ForceNoCull) {
                use_back_cull = false;
            }
            pipeline = use_back_cull ? impl_->pipeline_back_cull : impl_->pipeline_no_cull;
        }
        if (pipeline.id != current_pipeline.id) {
            sg_apply_pipeline(pipeline);
            sg_apply_uniforms(UB_scene_vs_params, SG_RANGE(vs_params));
            current_pipeline = pipeline;
        }

        scene_fs_params_t fs_params{};
        std::memcpy(fs_params.base_color, glm::value_ptr(gmat.base_color),
                    sizeof(fs_params.base_color));
        std::memcpy(fs_params.light_dir, glm::value_ptr(light_dir), sizeof(fs_params.light_dir));
        const float shader_cut_start = glm::radians(flags.angle_cut_start_deg);
        const float shader_cut_end = glm::radians(flags.angle_cut_end_deg);
        const bool large_cut = angleSpan(shader_cut_start, shader_cut_end) > 0.5f * kTau;
        // cut_params.z selects the LOD cross-fade dither half for this draw:
        // 1 = detail slabs, 2 = hull proxy, 0 = neither (Normal groups). The FS
        // dithers the two halves complementarily against a shared threshold; at
        // fade 0/1 the discard is a no-op, so this is safe even when LOD is off.
        const float lod_dither_role = g.lod_role == Impl::LodRole::Detail  ? 1.f
                                      : g.lod_role == Impl::LodRole::Proxy ? 2.f
                                                                           : 0.f;
        // cut_params.w carries the material-stack prefilter's band-width dial
        // (material_prefilter_band) -- global, like the angle-cut fields
        // above it, so this vec4 is the natural spare slot rather than adding
        // a whole new uniform for one float.
        const glm::vec4 cut_params{(flags.angle_cut && flags.shader_angle_cut) ? 1.f : 0.f,
                                   large_cut ? 1.f : 0.f, lod_dither_role,
                                   flags.material_prefilter_band};
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
        const glm::vec4 mode_flags{flags.enable_pbr ? 1.f : 0.f, prefilter_max_lod,
                                   overdraw_increment, flags.material_prefilter ? 1.f : 0.f};
        std::memcpy(fs_params.mode_flags, glm::value_ptr(mode_flags), sizeof(fs_params.mode_flags));
        // Per-mesh material-stack prefilter: average color + band width. w == 0
        // (untagged mesh) makes the scene FS skip the blend regardless of the
        // enable flag above.
        const glm::vec4 stack_prefilter{mesh.stack_avg_color,
                                        mesh.stack_feature_size * flags.material_prefilter_scale};
        std::memcpy(fs_params.stack_prefilter, glm::value_ptr(stack_prefilter),
                    sizeof(fs_params.stack_prefilter));
        const glm::vec3 eye = camera.eye();
        const glm::vec4 cam_pos{eye.x, eye.y, eye.z, 0.f};
        std::memcpy(fs_params.camera_pos, glm::value_ptr(cam_pos), sizeof(fs_params.camera_pos));
        std::memcpy(fs_params.emissive, glm::value_ptr(gmat.emissive), sizeof(fs_params.emissive));
        std::memcpy(fs_params.alpha_params, glm::value_ptr(gmat.alpha_params),
                    sizeof(fs_params.alpha_params));
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

sg_view SceneRenderer::iblPrefilterView() const { return impl_->ibl.prefilter_view; }

sg_sampler SceneRenderer::iblCubeSampler() const { return impl_->ibl.cube_sampler; }

} // namespace nodehammer::viewer
