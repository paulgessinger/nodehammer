// tinygltf implementation — defined in exactly this one TU.
// TINYGLTF_NO_STB_IMAGE / NO_STB_IMAGE_WRITE: we only write geometry, no embedded textures.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE

// tinygltf and its bundled nlohmann/json produce warnings in strict mode.
// Suppress them at the include site since they are not our code to fix.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-literal-operator"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <tiny_gltf.h>
#pragma GCC diagnostic pop

#include <nodehammer/export/gltf_exporter.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>

namespace nodehammer {

std::string_view GltfExporter::formatName() const noexcept { return "gltf"; }

std::vector<std::string> GltfExporter::supportedExtensions() const { return {".glb", ".gltf"}; }

ExportResult GltfExporter::write(const RenderScene &scene, const std::filesystem::path &path,
                                 const ExportConfig &config) const {
    ExportResult result;

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "nodehammer";

    tinygltf::Buffer buf;

    // Append raw bytes to the buffer; return starting byte offset.
    auto appendRaw = [&](const void *data, std::size_t size) -> std::size_t {
        const std::size_t off = buf.data.size();
        const auto *p = static_cast<const unsigned char *>(data);
        buf.data.insert(buf.data.end(), p, p + size);
        return off;
    };

    // Pad to 4-byte alignment (required for float and uint32 accessors).
    auto alignTo4 = [&]() {
        while (buf.data.size() % 4 != 0) {
            buf.data.push_back(0);
        }
    };

    // ── Materials ─────────────────────────────────────────────────────────────

    std::unordered_map<RenderMaterialId, int> matIdx;
    for (const auto &[id, mat] : scene.materials) {
        tinygltf::Material gm;
        gm.name = mat.name;
        gm.pbrMetallicRoughness.baseColorFactor = {
            static_cast<double>(mat.baseColorFactor.r),
            static_cast<double>(mat.baseColorFactor.g),
            static_cast<double>(mat.baseColorFactor.b),
            static_cast<double>(mat.baseColorFactor.a),
        };
        gm.pbrMetallicRoughness.metallicFactor = static_cast<double>(mat.metallicFactor);
        gm.pbrMetallicRoughness.roughnessFactor = static_cast<double>(mat.roughnessFactor);
        gm.emissiveFactor = {
            static_cast<double>(mat.emissiveFactor.r),
            static_cast<double>(mat.emissiveFactor.g),
            static_cast<double>(mat.emissiveFactor.b),
        };
        gm.doubleSided = mat.doubleSided;
        matIdx[id] = static_cast<int>(model.materials.size());
        model.materials.push_back(std::move(gm));
    }

    // ── Mesh assets → buffer + accessors ──────────────────────────────────────
    // One interleaved buffer view (stride = sizeof(Vertex)) per mesh asset.
    // POSITION offset = offsetof(Vertex, position), NORMAL offset = offsetof(Vertex, normal).

    struct MeshAccs {
        int pos, norm, idx;
    };
    std::unordered_map<MeshAssetId, MeshAccs> meshAccs;

    for (const auto &[id, ma] : scene.meshAssets) {
        if (ma.vertices.empty() || ma.indices.empty()) {
            continue;
        }

        // Vertex buffer view (interleaved POSITION + NORMAL)
        alignTo4();
        const std::size_t vtxOff =
            appendRaw(ma.vertices.data(), ma.vertices.size() * sizeof(Vertex));

        tinygltf::BufferView vtxBV;
        vtxBV.buffer = 0;
        vtxBV.byteOffset = vtxOff;
        vtxBV.byteLength = ma.vertices.size() * sizeof(Vertex);
        vtxBV.byteStride = sizeof(Vertex);
        vtxBV.target = TINYGLTF_TARGET_ARRAY_BUFFER;
        const int vtxBVIdx = static_cast<int>(model.bufferViews.size());
        model.bufferViews.push_back(vtxBV);

        // Index buffer view
        alignTo4();
        const std::size_t idxOff =
            appendRaw(ma.indices.data(), ma.indices.size() * sizeof(uint32_t));

        tinygltf::BufferView idxBV;
        idxBV.buffer = 0;
        idxBV.byteOffset = idxOff;
        idxBV.byteLength = ma.indices.size() * sizeof(uint32_t);
        idxBV.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
        const int idxBVIdx = static_cast<int>(model.bufferViews.size());
        model.bufferViews.push_back(idxBV);

        // Compute POSITION min/max (required by spec)
        glm::vec3 posMin{std::numeric_limits<float>::max()};
        glm::vec3 posMax{-std::numeric_limits<float>::max()};
        for (const auto &v : ma.vertices) {
            posMin = glm::min(posMin, v.position);
            posMax = glm::max(posMax, v.position);
        }

        // POSITION accessor
        tinygltf::Accessor posAcc;
        posAcc.bufferView = vtxBVIdx;
        posAcc.byteOffset = offsetof(Vertex, position);
        posAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        posAcc.type = TINYGLTF_TYPE_VEC3;
        posAcc.count = ma.vertices.size();
        posAcc.minValues = {static_cast<double>(posMin.x), static_cast<double>(posMin.y),
                            static_cast<double>(posMin.z)};
        posAcc.maxValues = {static_cast<double>(posMax.x), static_cast<double>(posMax.y),
                            static_cast<double>(posMax.z)};
        const int posAccIdx = static_cast<int>(model.accessors.size());
        model.accessors.push_back(posAcc);

        // NORMAL accessor
        tinygltf::Accessor normAcc;
        normAcc.bufferView = vtxBVIdx;
        normAcc.byteOffset = offsetof(Vertex, normal);
        normAcc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        normAcc.type = TINYGLTF_TYPE_VEC3;
        normAcc.count = ma.vertices.size();
        const int normAccIdx = static_cast<int>(model.accessors.size());
        model.accessors.push_back(normAcc);

        // INDEX accessor
        tinygltf::Accessor idxAcc;
        idxAcc.bufferView = idxBVIdx;
        idxAcc.byteOffset = 0;
        idxAcc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        idxAcc.type = TINYGLTF_TYPE_SCALAR;
        idxAcc.count = ma.indices.size();
        const int idxAccIdx = static_cast<int>(model.accessors.size());
        model.accessors.push_back(idxAcc);

        meshAccs[id] = {posAccIdx, normAccIdx, idxAccIdx};
    }

    // ── glTF meshes — one per unique (MeshAssetId, RenderMaterialId) ──────────
    // This lets nodes with the same (mesh, material) share a single glTF mesh.

    using MeshKey = std::pair<uint64_t, uint64_t>;
    std::map<MeshKey, int> meshKeyToGltf;

    auto getOrCreateGltfMesh = [&](const MeshBinding &binding) -> int {
        const MeshKey key{binding.meshId.value, binding.materialId.value};
        auto it = meshKeyToGltf.find(key);
        if (it != meshKeyToGltf.end()) {
            return it->second;
        }

        if (!meshAccs.contains(binding.meshId)) {
            return -1;
        }

        const auto &accs = meshAccs.at(binding.meshId);
        const int matI = matIdx.contains(binding.materialId) ? matIdx.at(binding.materialId) : -1;

        tinygltf::Primitive prim;
        prim.attributes["POSITION"] = accs.pos;
        prim.attributes["NORMAL"] = accs.norm;
        prim.indices = accs.idx;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
        prim.material = matI;

        tinygltf::Mesh gm;
        if (scene.meshAssets.contains(binding.meshId)) {
            gm.name = scene.meshAssets.at(binding.meshId).name;
        }
        gm.primitives.push_back(std::move(prim));

        const int meshI = static_cast<int>(model.meshes.size());
        model.meshes.push_back(std::move(gm));
        meshKeyToGltf[key] = meshI;
        return meshI;
    };

    // ── Nodes — BFS from root, assigning glTF node indices ────────────────────

    std::unordered_map<RenderNodeId, int> nodeIdx;

    if (scene.nodes.contains(scene.rootId)) {
        std::queue<RenderNodeId> q;
        q.push(scene.rootId);
        while (!q.empty()) {
            const RenderNodeId nid = q.front();
            q.pop();
            if (!scene.nodes.contains(nid) || nodeIdx.contains(nid)) {
                continue;
            }
            nodeIdx[nid] = static_cast<int>(model.nodes.size());
            model.nodes.emplace_back();
            for (const auto childId : scene.nodes.at(nid).children) {
                q.push(childId);
            }
        }
    }

    // Fill in node data now that all indices are known.
    for (const auto &[rnId, rn] : scene.nodes) {
        if (!nodeIdx.contains(rnId)) {
            continue;
        }
        tinygltf::Node &gn = model.nodes[static_cast<std::size_t>(nodeIdx.at(rnId))];
        gn.name = rn.name;

        // Local transform as column-major 4×4 matrix (GLM and glTF both column-major).
        // Apply unit_scale to the root node so the entire scene is rescaled.
        const glm::mat4 localWithScale =
            (!rn.parentId.has_value() && config.unitScale != 1.0)
                ? glm::mat4(glm::dmat4(rn.localTransform) *
                            glm::dmat4(glm::scale(glm::dmat4(1.0), glm::dvec3(config.unitScale))))
                : rn.localTransform;
        const glm::mat4 &m = localWithScale;
        gn.matrix = {
            static_cast<double>(m[0][0]), static_cast<double>(m[0][1]),
            static_cast<double>(m[0][2]), static_cast<double>(m[0][3]),
            static_cast<double>(m[1][0]), static_cast<double>(m[1][1]),
            static_cast<double>(m[1][2]), static_cast<double>(m[1][3]),
            static_cast<double>(m[2][0]), static_cast<double>(m[2][1]),
            static_cast<double>(m[2][2]), static_cast<double>(m[2][3]),
            static_cast<double>(m[3][0]), static_cast<double>(m[3][1]),
            static_cast<double>(m[3][2]), static_cast<double>(m[3][3]),
        };

        for (const auto childId : rn.children) {
            if (nodeIdx.contains(childId)) {
                gn.children.push_back(nodeIdx.at(childId));
            }
        }

        // First mesh binding → glTF node mesh. (Multiple bindings are rare in practice;
        // subsequent bindings would need separate child nodes — not implemented here.)
        if (!rn.meshBindings.empty()) {
            const int mi = getOrCreateGltfMesh(rn.meshBindings.front());
            if (mi >= 0) {
                gn.mesh = mi;
            }
        }
    }

    // ── Scene ─────────────────────────────────────────────────────────────────

    tinygltf::Scene gscene;
    gscene.name = "scene";
    if (nodeIdx.contains(scene.rootId)) {
        gscene.nodes = {nodeIdx.at(scene.rootId)};
    }
    model.scenes.push_back(std::move(gscene));
    model.defaultScene = 0;

    // ── Finalize buffer ───────────────────────────────────────────────────────

    model.buffers.push_back(std::move(buf));

    // ── Write ─────────────────────────────────────────────────────────────────

    const bool isBinary =
        (config.format == ExportConfig::Format::GLB) ||
        (config.format != ExportConfig::Format::GLTF && path.extension() == ".glb");

    tinygltf::TinyGLTF writer;
    std::string err;
    const bool ok = writer.WriteGltfSceneToFile(&model, path.string(),
                                                /*embedImages=*/true,
                                                /*prettyPrint=*/false,
                                                /*writeBinary=*/isBinary,
                                                /*isBinary=*/isBinary);
    if (!ok) {
        result.diags.error(codes::kErrExportWriteFailed,
                           std::format("failed to write glTF to '{}'", path.string()),
                           path.string());
    }

    (void)err;
    return result;
}

} // namespace nodehammer
