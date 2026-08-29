#include <ir/render_json.hpp>

// The Render codec serializes glm vectors/matrices (glm_json), the semantic::NodeId
// back-reference and Provenance (both from the Semantic IR's codec). Pulling these
// in here — rather than in render.hpp — is exactly what keeps the header lean.
#include <detail/glm_json.hpp>
#include <detail/overloaded.hpp>
#include <ir/id_order.hpp>
#include <ir/semantic_json.hpp>

namespace nodehammer::ir::render {

namespace {

/// ExtrasMap → nlohmann::json, for the `dump-render` output.
///
/// This is the one place the two still meet: extras stopped being nlohmann to
/// keep it out of render.hpp (detail/json_value.hpp explains why), but the rest
/// of this codec is nlohmann and there is no reason for it not to be — nothing
/// includes render_json.hpp except the TUs that already serialize.
nlohmann::json extrasToJson(const ExtrasMap &v) {
    return std::visit(detail::overloaded{
                          [](std::monostate) { return nlohmann::json{}; },
                          [](bool b) { return nlohmann::json(b); },
                          [](std::int64_t i) { return nlohmann::json(i); },
                          [](double d) { return nlohmann::json(d); },
                          [](const std::string &str) { return nlohmann::json(str); },
                          [](const ExtrasMap::Array &elems) {
                              auto arr = nlohmann::json::array();
                              for (const auto &elem : elems) {
                                  arr.push_back(extrasToJson(elem));
                              }
                              return arr;
                          },
                          [](const ExtrasMap::Object &members) {
                              auto obj = nlohmann::json::object();
                              for (const auto &[key, val] : members) {
                                  obj[key] = extrasToJson(val);
                              }
                              return obj;
                          },
                      },
                      v.value());
}

} // namespace

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

void to_json(nlohmann::json &j, const Material &m) {
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

void to_json(nlohmann::json &j, const Node &n) {
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
    if (!n.extras.empty())
        j["extras"] = extrasToJson(n.extras);
}

void to_json(nlohmann::json &j, const Scene &sc) {
    // By ID, not in map order: see ir/id_order.hpp.
    auto nodes = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.nodes))
        nodes.push_back(entry->second);

    auto meshes = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.meshAssets))
        meshes.push_back(entry->second);

    auto mats = nlohmann::json::array();
    for (const auto *entry : entriesById(sc.materials))
        mats.push_back(entry->second);

    j = {
        {"rootId", sc.rootId},
        {"nodes", nodes},
        {"meshAssets", meshes},
        {"materials", mats},
    };
}

} // namespace nodehammer::ir::render
