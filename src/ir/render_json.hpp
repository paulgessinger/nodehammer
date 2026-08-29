#pragma once

// JSON codec for the Render IR. Kept out of render.hpp so the many hot,
// non-JSON includers of the data model (tessellator, scene renderer, viewer)
// don't drag in the Semantic IR's JSON codec (semantic_json.hpp) or compile the
// inline to_json bodies. render.hpp is nlohmann-free -- render::ExtrasMap is
// detail::JsonValue -- so this header is the only thing that pulls the JSON
// library in for the Render IR. Only TUs that serialize render types include it.

#include <ir/render.hpp>

#include <nlohmann/json.hpp>

namespace nodehammer::ir::render {

void to_json(nlohmann::json &j, const Vertex &v);
void to_json(nlohmann::json &j, const MeshAsset &a);
void to_json(nlohmann::json &j, const Material &m);
void to_json(nlohmann::json &j, const MeshBinding &b);
void to_json(nlohmann::json &j, const Node &n);
void to_json(nlohmann::json &j, const Scene &sc);

} // namespace nodehammer::ir::render
