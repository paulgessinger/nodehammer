#include <nodehammer/ir/fb/render/flatbuffer.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace nodehammer {

namespace {

namespace fbr = fbs::render;

// Whole-array memcpy fast path for vertices relies on nodehammer::Vertex having
// the exact byte layout of fbr::Vertex (pos+normal, 6 contiguous floats). GLM's
// default vec3 is 12 bytes; if a build ever forces aligned gentypes this assert
// fires and the codec must switch to element-wise copies.
static_assert(sizeof(Vertex) == sizeof(fbr::Vertex),
              "Vertex layout must match fbs::render::Vertex for the memcpy fast path");
static_assert(sizeof(fbr::Vertex) == 24, "unexpected fbs::render::Vertex size");

// ── glm <-> fbs struct helpers ───────────────────────────────────────────────

fbr::Vec3f toVec3(const glm::vec3 &v) { return fbr::Vec3f{v.x, v.y, v.z}; }
fbr::Vec4f toVec4(const glm::vec4 &v) { return fbr::Vec4f{v.x, v.y, v.z, v.w}; }
fbr::Mat4f toMat4(const glm::mat4 &m) {
    return fbr::Mat4f{toVec4(m[0]), toVec4(m[1]), toVec4(m[2]), toVec4(m[3])};
}

glm::vec3 fromVec3(const fbr::Vec3f &v) { return {v.x(), v.y(), v.z()}; }
glm::vec4 fromVec4(const fbr::Vec4f &v) { return {v.x(), v.y(), v.z(), v.w()}; }
glm::mat4 fromMat4(const fbr::Mat4f &m) {
    glm::mat4 out{1.f};
    out[0] = fromVec4(m.c0());
    out[1] = fromVec4(m.c1());
    out[2] = fromVec4(m.c2());
    out[3] = fromVec4(m.c3());
    return out;
}

// ── serialize (Layer 1) ──────────────────────────────────────────────────────

flatbuffers::Offset<fbr::Provenance> serializeProvenance(flatbuffers::FlatBufferBuilder &b,
                                                         const Provenance &prov) {
    auto sys = b.CreateString(prov.sourceSystem);
    auto name = b.CreateString(prov.sourceName);
    auto file = b.CreateString(prov.sourceFile);
    const auto deg = static_cast<uint8_t>(prov.degradation.bits.to_ulong());
    fbr::ProvenanceBuilder pb(b);
    pb.add_source_system(sys);
    pb.add_source_name(name);
    pb.add_source_file(file);
    pb.add_degradation(deg);
    return pb.Finish();
}

flatbuffers::Offset<fbr::MeshAsset> serializeMeshAsset(flatbuffers::FlatBufferBuilder &b,
                                                       const MeshAsset &asset) {
    auto name = b.CreateString(asset.name);
    // Vertex layout matches fbr::Vertex (static_assert above), so the source
    // array is reinterpreted as fbs structs and copied wholesale.
    auto verts = b.CreateVectorOfStructs(
        reinterpret_cast<const fbr::Vertex *>(asset.vertices.data()), asset.vertices.size());
    auto indices = b.CreateVector(asset.indices);
    auto prov = serializeProvenance(b, asset.provenance);
    // Optional stack-average prefilter hint: build the sub-table first (a
    // nested table can't be open while the MeshAssetBuilder is), leave the
    // offset null when absent. Dropping this is what made the material-stack
    // prefilter a no-op in WASM.
    flatbuffers::Offset<fbr::StackAverage> stackAvg = 0;
    if (asset.stackAverage.has_value()) {
        const fbr::Vec3f avgColor = toVec3(asset.stackAverage->avgColorLinear);
        fbr::StackAverageBuilder sb(b);
        sb.add_avg_color_linear(&avgColor);
        sb.add_feature_size(asset.stackAverage->featureSize);
        stackAvg = sb.Finish();
    }
    fbr::MeshAssetBuilder mb(b);
    mb.add_id(asset.id.value);
    mb.add_name(name);
    mb.add_vertices(verts);
    mb.add_indices(indices);
    mb.add_provenance(prov);
    if (!stackAvg.IsNull()) {
        mb.add_stack_average(stackAvg);
    }
    return mb.Finish();
}

flatbuffers::Offset<fbr::RenderMaterial> serializeMaterial(flatbuffers::FlatBufferBuilder &b,
                                                           const RenderMaterial &mat) {
    auto name = b.CreateString(mat.name);
    auto alphaMode = b.CreateString(mat.alphaMode);
    const fbr::Vec4f baseColor = toVec4(mat.baseColorFactor);
    const fbr::Vec3f emissive = toVec3(mat.emissiveFactor);
    fbr::Vec3f specularColor{};
    fbr::RenderMaterialBuilder mb(b);
    mb.add_id(mat.id.value);
    mb.add_name(name);
    mb.add_base_color(&baseColor);
    mb.add_metallic(mat.metallicFactor);
    mb.add_roughness(mat.roughnessFactor);
    mb.add_emissive(&emissive);
    mb.add_alpha_mode(alphaMode);
    mb.add_alpha_cutoff(mat.alphaCutoff);
    mb.add_double_sided(mat.doubleSided);
    if (mat.ior) {
        mb.add_ior(*mat.ior);
    }
    if (mat.transmissionFactor) {
        mb.add_transmission(*mat.transmissionFactor);
    }
    if (mat.clearcoatFactor) {
        mb.add_clearcoat(*mat.clearcoatFactor);
    }
    if (mat.clearcoatRoughnessFactor) {
        mb.add_clearcoat_roughness(*mat.clearcoatRoughnessFactor);
    }
    if (mat.anisotropyStrength) {
        mb.add_anisotropy_strength(*mat.anisotropyStrength);
    }
    if (mat.anisotropyRotation) {
        mb.add_anisotropy_rotation(*mat.anisotropyRotation);
    }
    if (mat.specularFactor) {
        mb.add_specular(*mat.specularFactor);
    }
    if (mat.specularColorFactor) {
        specularColor = toVec3(*mat.specularColorFactor);
        mb.add_specular_color(&specularColor);
    }
    return mb.Finish();
}

flatbuffers::Offset<fbr::RenderNode> serializeNode(flatbuffers::FlatBufferBuilder &b,
                                                   const RenderNode &node) {
    auto name = b.CreateString(node.name);
    std::vector<uint64_t> childIds;
    childIds.reserve(node.children.size());
    for (const auto c : node.children) {
        childIds.push_back(c.value);
    }
    auto children = b.CreateVector(childIds);
    std::vector<fbr::MeshBinding> bindings;
    bindings.reserve(node.meshBindings.size());
    for (const auto &mb : node.meshBindings) {
        bindings.emplace_back(mb.meshId.value, mb.materialId.value);
    }
    auto meshBindings = b.CreateVectorOfStructs(bindings);
    std::vector<fbr::MeshBinding> proxyBindings;
    proxyBindings.reserve(node.lodProxyBindings.size());
    for (const auto &mb : node.lodProxyBindings) {
        proxyBindings.emplace_back(mb.meshId.value, mb.materialId.value);
    }
    auto lodProxyBindings = b.CreateVectorOfStructs(proxyBindings);
    const fbr::Mat4f local = toMat4(node.localTransform);
    const fbr::Mat4f world = toMat4(node.worldTransform);
    fbr::RenderNodeBuilder nb(b);
    nb.add_id(node.id.value);
    nb.add_name(name);
    nb.add_local_transform(&local);
    nb.add_world_transform(&world);
    nb.add_parent_id(node.parentId ? node.parentId->value : 0);
    nb.add_children(children);
    nb.add_mesh_bindings(meshBindings);
    nb.add_lod_proxy_bindings(lodProxyBindings);
    nb.add_semantic_node_id(node.semanticNodeId.value);
    return nb.Finish();
}

// ── deserialize (Layer 1) ────────────────────────────────────────────────────

Provenance deserializeProvenance(const fbr::Provenance *p) {
    Provenance prov;
    if (p == nullptr) {
        return prov;
    }
    if (p->source_system() != nullptr) {
        prov.sourceSystem = p->source_system()->str();
    }
    if (p->source_name() != nullptr) {
        prov.sourceName = p->source_name()->str();
    }
    if (p->source_file() != nullptr) {
        prov.sourceFile = p->source_file()->str();
    }
    prov.degradation.bits =
        decltype(prov.degradation.bits){static_cast<unsigned long long>(p->degradation())};
    return prov;
}

} // namespace

flatbuffers::Offset<fbr::RenderScene>
renderSceneToFlatBuffer(flatbuffers::FlatBufferBuilder &builder, const RenderScene &scene) {
    std::vector<flatbuffers::Offset<fbr::RenderNode>> nodeOffsets;
    nodeOffsets.reserve(scene.nodes.size());
    for (const auto &[id, node] : scene.nodes) {
        nodeOffsets.push_back(serializeNode(builder, node));
    }
    std::vector<flatbuffers::Offset<fbr::MeshAsset>> meshOffsets;
    meshOffsets.reserve(scene.meshAssets.size());
    for (const auto &[id, asset] : scene.meshAssets) {
        meshOffsets.push_back(serializeMeshAsset(builder, asset));
    }
    std::vector<flatbuffers::Offset<fbr::RenderMaterial>> matOffsets;
    matOffsets.reserve(scene.materials.size());
    for (const auto &[id, mat] : scene.materials) {
        matOffsets.push_back(serializeMaterial(builder, mat));
    }

    auto nodes = builder.CreateVector(nodeOffsets);
    auto meshes = builder.CreateVector(meshOffsets);
    auto materials = builder.CreateVector(matOffsets);

    fbr::RenderSceneBuilder sb(builder);
    sb.add_root_id(scene.rootId.value);
    sb.add_nodes(nodes);
    sb.add_mesh_assets(meshes);
    sb.add_materials(materials);
    return sb.Finish();
}

RenderScene renderSceneFromFlatBuffer(const fbr::RenderScene &fb) {
    RenderScene scene;
    scene.rootId = RenderNodeId{fb.root_id()};

    if (const auto *meshes = fb.mesh_assets(); meshes != nullptr) {
        for (const auto *m : *meshes) {
            MeshAsset asset;
            asset.id = MeshAssetId{m->id()};
            if (m->name() != nullptr) {
                asset.name = m->name()->str();
            }
            if (const auto *vs = m->vertices(); vs != nullptr) {
                asset.vertices.resize(vs->size());
                std::memcpy(asset.vertices.data(), vs->Data(),
                            static_cast<size_t>(vs->size()) * sizeof(Vertex));
            }
            if (const auto *is = m->indices(); is != nullptr) {
                asset.indices.resize(is->size());
                std::memcpy(asset.indices.data(), is->Data(),
                            static_cast<size_t>(is->size()) * sizeof(uint32_t));
            }
            asset.provenance = deserializeProvenance(m->provenance());
            if (const auto *sa = m->stack_average(); sa != nullptr) {
                StackAverage avg;
                if (const auto *c = sa->avg_color_linear(); c != nullptr) {
                    avg.avgColorLinear = fromVec3(*c);
                }
                avg.featureSize = sa->feature_size();
                asset.stackAverage = avg;
            }
            scene.meshAssets.emplace(asset.id, std::move(asset));
        }
    }

    if (const auto *materials = fb.materials(); materials != nullptr) {
        for (const auto *m : *materials) {
            RenderMaterial mat;
            mat.id = RenderMaterialId{m->id()};
            if (m->name() != nullptr) {
                mat.name = m->name()->str();
            }
            if (const auto *bc = m->base_color(); bc != nullptr) {
                mat.baseColorFactor = fromVec4(*bc);
            }
            mat.metallicFactor = m->metallic();
            mat.roughnessFactor = m->roughness();
            if (const auto *e = m->emissive(); e != nullptr) {
                mat.emissiveFactor = fromVec3(*e);
            }
            if (m->alpha_mode() != nullptr) {
                mat.alphaMode = m->alpha_mode()->str();
            }
            mat.alphaCutoff = m->alpha_cutoff();
            mat.doubleSided = m->double_sided();
            if (auto v = m->ior(); v) {
                mat.ior = *v;
            }
            if (auto v = m->transmission(); v) {
                mat.transmissionFactor = *v;
            }
            if (auto v = m->clearcoat(); v) {
                mat.clearcoatFactor = *v;
            }
            if (auto v = m->clearcoat_roughness(); v) {
                mat.clearcoatRoughnessFactor = *v;
            }
            if (auto v = m->anisotropy_strength(); v) {
                mat.anisotropyStrength = *v;
            }
            if (auto v = m->anisotropy_rotation(); v) {
                mat.anisotropyRotation = *v;
            }
            if (auto v = m->specular(); v) {
                mat.specularFactor = *v;
            }
            if (const auto *sc = m->specular_color(); sc != nullptr) {
                mat.specularColorFactor = fromVec3(*sc);
            }
            scene.materials.emplace(mat.id, std::move(mat));
        }
    }

    if (const auto *nodes = fb.nodes(); nodes != nullptr) {
        for (const auto *n : *nodes) {
            RenderNode node;
            node.id = RenderNodeId{n->id()};
            if (n->name() != nullptr) {
                node.name = n->name()->str();
            }
            if (const auto *lt = n->local_transform(); lt != nullptr) {
                node.localTransform = fromMat4(*lt);
            }
            if (const auto *wt = n->world_transform(); wt != nullptr) {
                node.worldTransform = fromMat4(*wt);
            }
            if (n->parent_id() != 0) {
                node.parentId = RenderNodeId{n->parent_id()};
            }
            if (const auto *ch = n->children(); ch != nullptr) {
                node.children.reserve(ch->size());
                for (const auto c : *ch) {
                    node.children.push_back(RenderNodeId{c});
                }
            }
            if (const auto *bs = n->mesh_bindings(); bs != nullptr) {
                node.meshBindings.reserve(bs->size());
                for (const auto *bnd : *bs) {
                    node.meshBindings.push_back(
                        {MeshAssetId{bnd->mesh_id()}, RenderMaterialId{bnd->material_id()}});
                }
            }
            if (const auto *ps = n->lod_proxy_bindings(); ps != nullptr) {
                node.lodProxyBindings.reserve(ps->size());
                for (const auto *bnd : *ps) {
                    node.lodProxyBindings.push_back(
                        {MeshAssetId{bnd->mesh_id()}, RenderMaterialId{bnd->material_id()}});
                }
            }
            node.semanticNodeId = SemanticNodeId{n->semantic_node_id()};
            scene.nodes.emplace(node.id, std::move(node));
        }
    }

    return scene;
}

// ── Layer 2: Byte buffer convenience ────────────────────────────────────────

std::vector<std::byte> renderSceneToBytes(const RenderScene &scene) {
    flatbuffers::FlatBufferBuilder builder{1024};
    auto root = renderSceneToFlatBuffer(builder, scene);
    fbr::FinishRenderSceneBuffer(builder, root);
    auto *ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    auto span = std::as_bytes(std::span{ptr, size});
    return std::vector<std::byte>(span.begin(), span.end());
}

RenderScene renderSceneFromBytes(std::span<const std::byte> buf) {
    const auto *ptr = reinterpret_cast<const uint8_t *>(buf.data());
    flatbuffers::Verifier verifier{ptr, buf.size()};
    if (!fbr::VerifyRenderSceneBuffer(verifier)) {
        throw std::runtime_error("FlatBuffer verification failed: invalid render buffer");
    }
    const auto *fb = fbr::GetRenderScene(ptr);
    return renderSceneFromFlatBuffer(*fb);
}

} // namespace nodehammer
