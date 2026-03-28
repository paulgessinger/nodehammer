#pragma once
// nlohmann::adl_serializer specializations for GLM types.
// Must be included before any translation unit that serializes glm types via nlohmann/json.

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace nlohmann {

template <> struct adl_serializer<glm::dvec3> {
    static void to_json(json &j, const glm::dvec3 &v) { j = {v.x, v.y, v.z}; }
};

template <> struct adl_serializer<glm::vec3> {
    static void to_json(json &j, const glm::vec3 &v) { j = {v.x, v.y, v.z}; }
};

template <> struct adl_serializer<glm::vec4> {
    static void to_json(json &j, const glm::vec4 &v) { j = {v.x, v.y, v.z, v.w}; }
};

template <> struct adl_serializer<glm::dmat4> {
    static void to_json(json &j, const glm::dmat4 &m) {
        j = json::array();
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                j.push_back(m[col][row]);
    }
};

template <> struct adl_serializer<glm::mat4> {
    static void to_json(json &j, const glm::mat4 &m) {
        j = json::array();
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                j.push_back(m[col][row]);
    }
};

} // namespace nlohmann
