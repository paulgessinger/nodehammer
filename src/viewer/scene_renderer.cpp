#include <nodehammer/viewer/scene_renderer.hpp>

#include <nodehammer/viewer/camera.hpp>

// Match what app.cpp does: suppress profiles we don't compile so
// BGFX_EMBEDDED_SHADER doesn't reference missing symbols.
#define BGFX_PLATFORM_SUPPORTS_DXBC 0
#define BGFX_PLATFORM_SUPPORTS_DXIL 0
#define BGFX_PLATFORM_SUPPORTS_NVN 0
#define BGFX_PLATFORM_SUPPORTS_PSSL 0
#define BGFX_PLATFORM_SUPPORTS_WGSL 0

#include <ankerl/unordered_dense.h>
#include <bgfx/bgfx.h>
#include <bgfx/embedded_shader.h>
#include <bx/platform.h>
#include <glm/gtc/type_ptr.hpp>
#include <nodehammer/ir/render.hpp>

#if BGFX_PLATFORM_SUPPORTS_GLSL
#include "glsl/fs_scene_lambert.sc.bin.h"
#include "glsl/vs_scene.sc.bin.h"
#endif
#if BGFX_PLATFORM_SUPPORTS_ESSL
#include "essl/fs_scene_lambert.sc.bin.h"
#include "essl/vs_scene.sc.bin.h"
#endif
#if BGFX_PLATFORM_SUPPORTS_SPIRV
#include "spirv/fs_scene_lambert.sc.bin.h"
#include "spirv/vs_scene.sc.bin.h"
#endif
#if BGFX_PLATFORM_SUPPORTS_METAL
#include "metal/fs_scene_lambert.sc.bin.h"
#include "metal/vs_scene.sc.bin.h"
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace nodehammer::viewer {

namespace {

const bgfx::EmbeddedShader k_scene_shaders[] = {
    BGFX_EMBEDDED_SHADER(vs_scene),
    BGFX_EMBEDDED_SHADER(fs_scene_lambert),
    BGFX_EMBEDDED_SHADER_END(),
};

bgfx::VertexLayout scene_vertex_layout() {
    bgfx::VertexLayout l;
    l.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .end();
    return l;
}

struct GpuMesh {
    bgfx::VertexBufferHandle vbh = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle ibh = BGFX_INVALID_HANDLE;
    uint32_t index_count{0};
    uint64_t triangle_count{0};
    glm::vec3 local_min{0.f};
    glm::vec3 local_max{0.f};
};

/// Transform an AABB by a 4x4 affine matrix, producing a fresh AABB that
/// bounds the result. Cheaper than transforming all 8 corners — Arvo's
/// trick: each axis of the output range is built from positive/negative
/// contributions of each row of the matrix.
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

struct DrawItem {
    MeshAssetId mesh;
    RenderMaterialId material;
    glm::mat4 world;
};

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

} // namespace

struct SceneRenderer::Impl {
    bool initialised{false};
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_baseColor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle u_lightDir = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout vertex_layout;

    ankerl::unordered_dense::map<MeshAssetId, GpuMesh> meshes;
    ankerl::unordered_dense::map<RenderMaterialId, glm::vec4> material_colors;

    // Flattened draw plan: groups of (mesh, material) with N world matrices.
    struct DrawGroup {
        MeshAssetId mesh;
        RenderMaterialId material;
        std::vector<glm::mat4> instances;
    };
    std::vector<DrawGroup> groups;

    // Scene-wide stats.
    uint32_t node_count{0};
    uint64_t triangle_count{0};
    glm::vec3 bounds_min{0.f};
    glm::vec3 bounds_max{0.f};
    bool has_bounds{false};

    FrameStats last_stats;
    bool caps_instancing{false};

    void ensure_init();
    void destroy_gpu();
};

void SceneRenderer::Impl::ensure_init() {
    if (initialised) {
        return;
    }
    vertex_layout = scene_vertex_layout();
    const bgfx::RendererType::Enum type = bgfx::getRendererType();
    program = bgfx::createProgram(
        bgfx::createEmbeddedShader(k_scene_shaders, type, "vs_scene"),
        bgfx::createEmbeddedShader(k_scene_shaders, type, "fs_scene_lambert"), true);
    u_baseColor = bgfx::createUniform("u_baseColor", bgfx::UniformType::Vec4);
    u_lightDir = bgfx::createUniform("u_lightDir", bgfx::UniformType::Vec4);
    caps_instancing = (bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING) != 0;
    initialised = true;
    if (!caps_instancing) {
        std::fprintf(stderr, "viewer: BGFX_CAPS_INSTANCING is not reported by this renderer; "
                             "the scene renderer requires it (vs_scene reads i_data0..3).\n");
    }
}

void SceneRenderer::Impl::destroy_gpu() {
    for (auto &[id, m] : meshes) {
        if (bgfx::isValid(m.vbh)) {
            bgfx::destroy(m.vbh);
        }
        if (bgfx::isValid(m.ibh)) {
            bgfx::destroy(m.ibh);
        }
    }
    meshes.clear();
    groups.clear();
    material_colors.clear();
    node_count = 0;
    triangle_count = 0;
    has_bounds = false;
}

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}

SceneRenderer::~SceneRenderer() {
    // Intentionally empty: bgfx handles must be released BEFORE bgfx::shutdown,
    // and only the App lifecycle knows that ordering. Caller invokes release()
    // at the right point; if not, bgfx::shutdown reaps the handles anyway.
}

void SceneRenderer::release() {
    if (!impl_ || !impl_->initialised) {
        return;
    }
    impl_->destroy_gpu();
    if (bgfx::isValid(impl_->program)) {
        bgfx::destroy(impl_->program);
        impl_->program = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(impl_->u_baseColor)) {
        bgfx::destroy(impl_->u_baseColor);
        impl_->u_baseColor = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(impl_->u_lightDir)) {
        bgfx::destroy(impl_->u_lightDir);
        impl_->u_lightDir = BGFX_INVALID_HANDLE;
    }
    impl_->initialised = false;
}

void SceneRenderer::upload(const RenderScene &scene) {
    impl_->ensure_init();
    impl_->destroy_gpu();

    static_assert(sizeof(Vertex) == 24, "Vertex layout must match bgfx 6f stride");

    // Upload mesh assets verbatim — the IR's vertex format already matches
    // bgfx's interleaved (pos vec3, normal vec3) layout. Also stash the
    // local AABB so we can compute the scene-wide world AABB during the
    // node walk below.
    for (const auto &[id, asset] : scene.meshAssets) {
        if (asset.vertices.empty() || asset.indices.empty()) {
            continue;
        }
        GpuMesh gm;
        // Use bgfx::copy (not makeRef) — bgfx may defer the actual GPU upload
        // and the source vectors must outlive the buffer for makeRef to be
        // safe. Copy makes the lifetime concern go away at the cost of holding
        // the data twice during upload.
        gm.vbh = bgfx::createVertexBuffer(
            bgfx::copy(asset.vertices.data(),
                       static_cast<uint32_t>(asset.vertices.size() * sizeof(Vertex))),
            impl_->vertex_layout);
        gm.ibh = bgfx::createIndexBuffer(
            bgfx::copy(asset.indices.data(),
                       static_cast<uint32_t>(asset.indices.size() * sizeof(uint32_t))),
            BGFX_BUFFER_INDEX32);
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

    // Pre-cache material base colours.
    for (const auto &[id, mat] : scene.materials) {
        impl_->material_colors.emplace(id, mat.baseColorFactor);
    }

    // Flatten the node hierarchy into a draw list, grouped by (mesh, material).
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
                impl_->groups.push_back({binding.meshId, binding.materialId, {}});
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

    std::fprintf(stderr,
                 "viewer: scene uploaded — meshes=%zu materials=%zu nodes=%u draws/groups=%zu "
                 "tris=%llu bbox=[%.1f %.1f %.1f .. %.1f %.1f %.1f]\n",
                 impl_->meshes.size(), impl_->material_colors.size(), impl_->node_count,
                 impl_->groups.size(), static_cast<unsigned long long>(impl_->triangle_count),
                 bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);

    // Diagnostic: dump the first non-empty group so we can sanity-check the
    // matrix layout going into the instance buffer and the vertex data coming
    // out of the IR. Helpful when "garbled visuals" point at a layout mismatch.
    for (const auto &g : impl_->groups) {
        if (g.instances.empty()) {
            continue;
        }
        const auto mit = impl_->meshes.find(g.mesh);
        if (mit == impl_->meshes.end()) {
            continue;
        }
        const auto &m0 = g.instances.front();
        std::fprintf(stderr,
                     "viewer:   sample group: mesh=%llu mat=%llu instances=%zu tris/inst=%llu\n",
                     static_cast<unsigned long long>(g.mesh.value),
                     static_cast<unsigned long long>(g.material.value), g.instances.size(),
                     static_cast<unsigned long long>(mit->second.triangle_count));
        std::fprintf(stderr,
                     "viewer:   first world (col-major):\n"
                     "    %8.3f %8.3f %8.3f %8.3f\n"
                     "    %8.3f %8.3f %8.3f %8.3f\n"
                     "    %8.3f %8.3f %8.3f %8.3f\n"
                     "    %8.3f %8.3f %8.3f %8.3f\n",
                     m0[0][0], m0[1][0], m0[2][0], m0[3][0], m0[0][1], m0[1][1], m0[2][1], m0[3][1],
                     m0[0][2], m0[1][2], m0[2][2], m0[3][2], m0[0][3], m0[1][3], m0[2][3],
                     m0[3][3]);
        std::fprintf(stderr, "viewer:   mesh local bbox: [%.3f %.3f %.3f .. %.3f %.3f %.3f]\n",
                     mit->second.local_min.x, mit->second.local_min.y, mit->second.local_min.z,
                     mit->second.local_max.x, mit->second.local_max.y, mit->second.local_max.z);
        break;
    }
}

void SceneRenderer::render(uint16_t view_id, const Camera &camera, uint32_t fb_width,
                           uint32_t fb_height, RenderFlags flags) {
    impl_->last_stats = {};
    if (impl_->groups.empty() || !bgfx::isValid(impl_->program)) {
        return;
    }
    if (fb_width == 0 || fb_height == 0) {
        return;
    }

    const bgfx::Caps *caps = bgfx::getCaps();
    const float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);
    const glm::mat4 view = camera.view();
    const glm::mat4 proj = camera.proj(aspect, caps->homogeneousDepth);
    bgfx::setViewTransform(view_id, glm::value_ptr(view), glm::value_ptr(proj));

    const float light_dir[4] = {-0.4f, -0.7f, -0.6f, 0.f};
    bgfx::setUniform(impl_->u_lightDir, light_dir);

    uint64_t draw_state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                          BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA;
    if (flags.cull_back) {
        // bgfx winding default is CW; CULL_CW eliminates back-facing triangles
        // for meshes with CCW-front-facing data. ODD's tessellator emits CCW
        // for outward-facing triangles, so this is the right cull direction.
        draw_state |= BGFX_STATE_CULL_CW;
    }
    // Note: BGFX_STATE_PT_LINES intentionally NOT exposed as a wireframe mode.
    // Reading a triangle index buffer as line pairs yields fake lines between
    // unrelated vertices (every 3rd line is "last vertex of triangle N to
    // first vertex of triangle N+1"), producing a misleading radial-streaming
    // pattern that masks whether the actual geometry is rendering correctly.
    // Real wireframe needs a per-mesh edge index buffer, deferred for later.
    (void)flags.wireframe;

    if (!impl_->caps_instancing) {
        return;
    }

    // vs_scene reads the world matrix from i_data0..3, so every group goes
    // through the instance buffer path — even a singleton becomes a 1-instance
    // submission. This keeps the shader signature the same on every draw.
    constexpr uint16_t k_stride = sizeof(glm::mat4);
    for (const auto &g : impl_->groups) {
        auto mesh_it = impl_->meshes.find(g.mesh);
        if (mesh_it == impl_->meshes.end()) {
            continue;
        }
        auto col_it = impl_->material_colors.find(g.material);
        glm::vec4 base = (col_it != impl_->material_colors.end())
                             ? col_it->second
                             : glm::vec4{0.8f, 0.8f, 0.8f, 1.f};
        bgfx::setUniform(impl_->u_baseColor, glm::value_ptr(base));

        const auto &mesh = mesh_it->second;
        const uint32_t n = static_cast<uint32_t>(g.instances.size());
        const uint32_t avail = bgfx::getAvailInstanceDataBuffer(n, k_stride);
        const uint32_t take = std::min(n, avail);
        if (take == 0) {
            continue;
        }
        bgfx::InstanceDataBuffer idb;
        bgfx::allocInstanceDataBuffer(&idb, take, k_stride);
        std::memcpy(idb.data, g.instances.data(), static_cast<size_t>(take) * k_stride);
        bgfx::setVertexBuffer(0, mesh.vbh);
        bgfx::setIndexBuffer(mesh.ibh);
        bgfx::setInstanceDataBuffer(&idb);
        bgfx::setState(draw_state);
        bgfx::submit(view_id, impl_->program);
        ++impl_->last_stats.draw_calls;
        impl_->last_stats.instances += take;
        impl_->last_stats.triangles += mesh.triangle_count * take;
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
