// tinygltf implementation — defined in exactly this one TU.
// TINYGLTF_NO_STB_IMAGE / NO_STB_IMAGE_WRITE: we only write geometry, no embedded textures.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE

// tinygltf and its bundled nlohmann/json produce warnings in strict mode.
// Suppress them at the include site since they are not our code to fix.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include <tiny_gltf.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <nodehammer/ir/gltf/render/exporter.hpp>

#include <nodehammer/ir/diagnostic_codes.hpp>
#include <set>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <queue>
#include <unordered_map>

namespace nodehammer {

namespace {

/// Recursively convert nlohmann::json to tinygltf::Value.
tinygltf::Value jsonToGltfValue(const nlohmann::json &j) {
    if (j.is_boolean()) {
        return tinygltf::Value(j.get<bool>());
    }
    if (j.is_number_integer()) {
        return tinygltf::Value(static_cast<int>(j.get<int64_t>()));
    }
    if (j.is_number_float()) {
        return tinygltf::Value(j.get<double>());
    }
    if (j.is_string()) {
        return tinygltf::Value(j.get<std::string>());
    }
    if (j.is_array()) {
        tinygltf::Value::Array arr;
        for (const auto &elem : j) {
            arr.push_back(jsonToGltfValue(elem));
        }
        return tinygltf::Value(std::move(arr));
    }
    if (j.is_object()) {
        tinygltf::Value::Object obj;
        for (const auto &[k, v] : j.items()) {
            obj[k] = jsonToGltfValue(v);
        }
        return tinygltf::Value(std::move(obj));
    }
    return {};
}

/// Convert RenderExtrasMap (nlohmann::json) to a tinygltf::Value for extras.
tinygltf::Value extrasToValue(const detail::RenderExtrasMap &extras) {
    return jsonToGltfValue(extras);
}

} // namespace

std::string_view GltfExporter::formatName() const noexcept { return "gltf"; }

std::vector<std::string> GltfExporter::supportedExtensions() const { return {".glb", ".gltf"}; }

ExportResult GltfExporter::write(const detail::RenderScene &scene,
                                 const std::filesystem::path &path,
                                 const ExportConfig &config) const {
    ExportResult result;

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "nodehammer";

    // Collect KHR extensions used by any material.
    {
        std::set<std::string> usedExts;
        for (const auto &[_, mat] : scene.materials) {
            if (mat.ior.has_value()) {
                usedExts.insert("KHR_materials_ior");
            }
            if (mat.transmissionFactor.has_value()) {
                usedExts.insert("KHR_materials_transmission");
            }
            if (mat.clearcoatFactor.has_value()) {
                usedExts.insert("KHR_materials_clearcoat");
            }
            if (mat.anisotropyStrength.has_value()) {
                usedExts.insert("KHR_materials_anisotropy");
            }
            if (mat.specularFactor.has_value() || mat.specularColorFactor.has_value()) {
                usedExts.insert("KHR_materials_specular");
            }
        }
        for (auto &ext : usedExts) {
            model.extensionsUsed.push_back(ext);
        }
    }

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
        gm.alphaMode = mat.alphaMode;
        gm.alphaCutoff = static_cast<double>(mat.alphaCutoff);
        gm.doubleSided = mat.doubleSided;

        if (mat.ior.has_value()) {
            tinygltf::Value::Object iorExt;
            iorExt["ior"] = tinygltf::Value(static_cast<double>(*mat.ior));
            gm.extensions["KHR_materials_ior"] = tinygltf::Value(iorExt);
        }
        if (mat.transmissionFactor.has_value()) {
            tinygltf::Value::Object transExt;
            transExt["transmissionFactor"] =
                tinygltf::Value(static_cast<double>(*mat.transmissionFactor));
            gm.extensions["KHR_materials_transmission"] = tinygltf::Value(transExt);
        }
        if (mat.clearcoatFactor.has_value()) {
            tinygltf::Value::Object ccExt;
            ccExt["clearcoatFactor"] = tinygltf::Value(static_cast<double>(*mat.clearcoatFactor));
            if (mat.clearcoatRoughnessFactor.has_value()) {
                ccExt["clearcoatRoughnessFactor"] =
                    tinygltf::Value(static_cast<double>(*mat.clearcoatRoughnessFactor));
            }
            gm.extensions["KHR_materials_clearcoat"] = tinygltf::Value(ccExt);
        }

        if (mat.anisotropyStrength.has_value()) {
            tinygltf::Value::Object anisExt;
            anisExt["anisotropyStrength"] =
                tinygltf::Value(static_cast<double>(*mat.anisotropyStrength));
            if (mat.anisotropyRotation.has_value()) {
                anisExt["anisotropyRotation"] =
                    tinygltf::Value(static_cast<double>(*mat.anisotropyRotation));
            }
            gm.extensions["KHR_materials_anisotropy"] = tinygltf::Value(anisExt);
        }
        if (mat.specularFactor.has_value() || mat.specularColorFactor.has_value()) {
            tinygltf::Value::Object specExt;
            if (mat.specularFactor.has_value()) {
                specExt["specularFactor"] =
                    tinygltf::Value(static_cast<double>(*mat.specularFactor));
            }
            if (mat.specularColorFactor.has_value()) {
                const auto &c = *mat.specularColorFactor;
                tinygltf::Value::Array arr;
                arr.push_back(tinygltf::Value(static_cast<double>(c.r)));
                arr.push_back(tinygltf::Value(static_cast<double>(c.g)));
                arr.push_back(tinygltf::Value(static_cast<double>(c.b)));
                specExt["specularColorFactor"] = tinygltf::Value(arr);
            }
            gm.extensions["KHR_materials_specular"] = tinygltf::Value(specExt);
        }

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

    const bool bake = config.bakeUnitScale;
    const auto s = static_cast<float>(config.unitScale);

    for (const auto &[id, ma] : scene.meshAssets) {
        if (ma.vertices.empty() || ma.indices.empty()) {
            continue;
        }

        // When baking, scale vertex positions into the target unit system.
        std::vector<detail::Vertex> scaledVerts;
        const std::vector<detail::Vertex> &verts = bake ? scaledVerts : ma.vertices;
        if (bake) {
            scaledVerts.reserve(ma.vertices.size());
            for (const auto &v : ma.vertices) {
                scaledVerts.push_back({v.position * s, v.normal});
            }
        }

        // Vertex buffer view (interleaved POSITION + NORMAL)
        alignTo4();
        const std::size_t vtxOff = appendRaw(verts.data(), verts.size() * sizeof(detail::Vertex));

        tinygltf::BufferView vtxBV;
        vtxBV.buffer = 0;
        vtxBV.byteOffset = vtxOff;
        vtxBV.byteLength = verts.size() * sizeof(detail::Vertex);
        vtxBV.byteStride = sizeof(detail::Vertex);
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
        for (const auto &v : verts) {
            posMin = glm::min(posMin, v.position);
            posMax = glm::max(posMax, v.position);
        }

        // POSITION accessor
        tinygltf::Accessor posAcc;
        posAcc.bufferView = vtxBVIdx;
        posAcc.byteOffset = offsetof(detail::Vertex, position);
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
        normAcc.byteOffset = offsetof(detail::Vertex, normal);
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
        glm::mat4 m = rn.localTransform;
        if (bake) {
            // Scale the translation component of every node's matrix.
            m[3] = glm::vec4(glm::vec3(m[3]) * s, m[3].w);
        } else if (!rn.parentId.has_value()) {
            // Non-bake: apply unit_scale to the root node so the entire scene is rescaled.
            m = glm::mat4(glm::dmat4(m) *
                          glm::dmat4(glm::scale(glm::dmat4(1.0), glm::dvec3(config.unitScale))));
        }
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

        // Map mesh bindings to a glTF mesh. A single binding uses the shared
        // mesh cache; multiple bindings (from merge_descendants with mixed
        // materials) produce a multi-primitive glTF mesh.
        if (rn.meshBindings.size() == 1) {
            const int mi = getOrCreateGltfMesh(rn.meshBindings.front());
            if (mi >= 0) {
                gn.mesh = mi;
            }
        } else if (rn.meshBindings.size() > 1) {
            tinygltf::Mesh gm;
            gm.name = rn.name;
            for (const auto &binding : rn.meshBindings) {
                if (!meshAccs.contains(binding.meshId)) {
                    continue;
                }
                const auto &accs = meshAccs.at(binding.meshId);
                const int matI =
                    matIdx.contains(binding.materialId) ? matIdx.at(binding.materialId) : -1;
                tinygltf::Primitive prim;
                prim.attributes["POSITION"] = accs.pos;
                prim.attributes["NORMAL"] = accs.norm;
                prim.indices = accs.idx;
                prim.mode = TINYGLTF_MODE_TRIANGLES;
                prim.material = matI;
                gm.primitives.push_back(std::move(prim));
            }
            if (!gm.primitives.empty()) {
                gn.mesh = static_cast<int>(model.meshes.size());
                model.meshes.push_back(std::move(gm));
            }
        }

        if (!rn.extras.is_null() && !rn.extras.empty()) {
            gn.extras = extrasToValue(rn.extras);
        }
    }

    // ── Scene(s) ──────────────────────────────────────────────────────────────

    if (config.gltf.multiScene) {
        // Build name path for each render node by walking parent chain.
        const auto &sep = config.gltf.sceneNameSeparator;
        std::unordered_map<RenderNodeId, std::string> namePaths;

        // BFS to build paths (parents before children).
        {
            std::queue<RenderNodeId> pq;
            if (scene.nodes.contains(scene.rootId)) {
                pq.push(scene.rootId);
            }
            while (!pq.empty()) {
                const auto nid = pq.front();
                pq.pop();
                if (!scene.nodes.contains(nid)) {
                    continue;
                }
                const auto &rn = scene.nodes.at(nid);
                if (!rn.parentId.has_value()) {
                    // Root node: empty prefix so children start without "world > ".
                    namePaths[nid] = "";
                } else if (!namePaths.contains(*rn.parentId) ||
                           namePaths.at(*rn.parentId).empty()) {
                    namePaths[nid] = rn.name;
                } else {
                    const auto &parentPath = namePaths.at(*rn.parentId);
                    namePaths[nid] = parentPath + sep + rn.name;
                }
                for (const auto childId : rn.children) {
                    pq.push(childId);
                }
            }
        }

        // Create one scene per mesh-bearing node.
        for (const auto &[rnId, rn] : scene.nodes) {
            if (rn.meshBindings.empty() || !nodeIdx.contains(rnId)) {
                continue;
            }

            // Create a new standalone node with world transform for this scene.
            tinygltf::Node sn;
            sn.name = rn.name;

            // Use world transform (scaled if baking).
            glm::mat4 wm = rn.worldTransform;
            if (bake) {
                wm[3] = glm::vec4(glm::vec3(wm[3]) * s, wm[3].w);
            } else {
                wm = glm::mat4(
                    glm::dmat4(wm) *
                    glm::dmat4(glm::scale(glm::dmat4(1.0), glm::dvec3(config.unitScale))));
            }
            sn.matrix = {
                static_cast<double>(wm[0][0]), static_cast<double>(wm[0][1]),
                static_cast<double>(wm[0][2]), static_cast<double>(wm[0][3]),
                static_cast<double>(wm[1][0]), static_cast<double>(wm[1][1]),
                static_cast<double>(wm[1][2]), static_cast<double>(wm[1][3]),
                static_cast<double>(wm[2][0]), static_cast<double>(wm[2][1]),
                static_cast<double>(wm[2][2]), static_cast<double>(wm[2][3]),
                static_cast<double>(wm[3][0]), static_cast<double>(wm[3][1]),
                static_cast<double>(wm[3][2]), static_cast<double>(wm[3][3]),
            };

            if (rn.meshBindings.size() == 1) {
                const int mi = getOrCreateGltfMesh(rn.meshBindings.front());
                if (mi >= 0) {
                    sn.mesh = mi;
                }
            } else if (rn.meshBindings.size() > 1) {
                tinygltf::Mesh gm;
                gm.name = rn.name;
                for (const auto &binding : rn.meshBindings) {
                    if (!meshAccs.contains(binding.meshId)) {
                        continue;
                    }
                    const auto &accs = meshAccs.at(binding.meshId);
                    const int matI =
                        matIdx.contains(binding.materialId) ? matIdx.at(binding.materialId) : -1;
                    tinygltf::Primitive prim;
                    prim.attributes["POSITION"] = accs.pos;
                    prim.attributes["NORMAL"] = accs.norm;
                    prim.indices = accs.idx;
                    prim.mode = TINYGLTF_MODE_TRIANGLES;
                    prim.material = matI;
                    gm.primitives.push_back(std::move(prim));
                }
                if (!gm.primitives.empty()) {
                    sn.mesh = static_cast<int>(model.meshes.size());
                    model.meshes.push_back(std::move(gm));
                }
            }

            const int snIdx = static_cast<int>(model.nodes.size());
            model.nodes.push_back(std::move(sn));

            tinygltf::Scene gs;
            gs.name = namePaths.contains(rnId) ? namePaths.at(rnId) : rn.name;
            gs.nodes = {snIdx};
            if (!rn.extras.is_null() && !rn.extras.empty()) {
                gs.extras = extrasToValue(rn.extras);
            }
            model.scenes.push_back(std::move(gs));
        }
        model.defaultScene = 0;
    } else {
        tinygltf::Scene gscene;
        gscene.name = "scene";
        if (nodeIdx.contains(scene.rootId)) {
            gscene.nodes = {nodeIdx.at(scene.rootId)};
        }
        model.scenes.push_back(std::move(gscene));
        model.defaultScene = 0;
    }

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
