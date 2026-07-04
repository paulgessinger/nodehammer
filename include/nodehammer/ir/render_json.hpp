#pragma once

// JSON codec for the Render IR. Kept out of render.hpp so the many hot,
// non-JSON includers of the data model (tessellator, scene renderer, viewer)
// don't drag in the Semantic IR's JSON codec (semantic_json.hpp) or compile the
// inline to_json bodies. (render.hpp still includes nlohmann/json itself, since
// RenderExtrasMap aliases it.) Only TUs that serialize render types include this.

#include <nodehammer/ir/render.hpp>

#include <nlohmann/json.hpp>

namespace nodehammer {

void to_json(nlohmann::json &j, const Vertex &v);
void to_json(nlohmann::json &j, const MeshAsset &a);
void to_json(nlohmann::json &j, const RenderMaterial &m);
void to_json(nlohmann::json &j, const MeshBinding &b);
void to_json(nlohmann::json &j, const RenderNode &n);
void to_json(nlohmann::json &j, const RenderScene &sc);

} // namespace nodehammer
