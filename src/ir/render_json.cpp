#include <nodehammer/ir/render_json.hpp>

// The Render codec serializes glm vectors/matrices (glm_json), the SemanticNodeId
// back-reference and Provenance (both from the Semantic IR's codec). Pulling these
// in here — rather than in render.hpp — is exactly what keeps the header lean.
#include <nodehammer/detail/glm_json.hpp>
#include <nodehammer/ir/semantic_json.hpp>

namespace nodehammer {

void to_json(nlohmann::json &j, const Vertex &v) {
    j = {
        {"position", {v.position.x, v.position.y, v.position.z}},
        {"normal", {v.normal.x, v.normal.y, v.normal.z}},
    };
}

void to_json(nlohmann::json &j, const MeshAsset &a) {
    j = {
        {"id", a.id},
        {"name", a.name},
        {"vertexCount", a.vertices.size()},
        {"indexCount", a.indices.size()},
        {"provenance", a.provenance},
    };
}

void to_json(nlohmann::json &j, const RenderMaterial &m) {
    j = {
        {"id", m.id},
        {"name", m.name},
        {"baseColorFactor", m.baseColorFactor},
        {"metallicFactor", m.metallicFactor},
        {"roughnessFactor", m.roughnessFactor},
        {"emissiveFactor", m.emissiveFactor},
        {"doubleSided", m.doubleSided},
    };
}

void to_json(nlohmann::json &j, const MeshBinding &b) {
    j = {{"meshId", b.meshId}, {"materialId", b.materialId}};
}

void to_json(nlohmann::json &j, const RenderNode &n) {
    j = {
        {"id", n.id},
        {"name", n.name},
        {"localTransform", n.localTransform},
        {"worldTransform", n.worldTransform},
        {"children", n.children},
        {"meshBindings", n.meshBindings},
        {"semanticNodeId", n.semanticNodeId},
    };
    if (n.parentId)
        j["parentId"] = *n.parentId;
    if (!n.extras.is_null() && !n.extras.empty())
        j["extras"] = n.extras;
}

void to_json(nlohmann::json &j, const RenderScene &sc) {
    auto nodes = nlohmann::json::array();
    for (const auto &[id, n] : sc.nodes)
        nodes.push_back(n);

    auto meshes = nlohmann::json::array();
    for (const auto &[id, m] : sc.meshAssets)
        meshes.push_back(m);

    auto mats = nlohmann::json::array();
    for (const auto &[id, m] : sc.materials)
        mats.push_back(m);

    j = {
        {"rootId", sc.rootId},
        {"nodes", nodes},
        {"meshAssets", meshes},
        {"materials", mats},
    };
}

} // namespace nodehammer
