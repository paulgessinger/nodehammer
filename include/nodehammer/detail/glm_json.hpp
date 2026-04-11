#pragma once
// nlohmann::adl_serializer specializations for GLM types.
// Must be included before any translation unit that serializes glm types via nlohmann/json.

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace nlohmann {

template <> struct adl_serializer<glm::dvec3> {
    static void to_json(json &j, const glm::dvec3 &v) { j = {v.x, v.y, v.z}; }
    static void from_json(const json &j, glm::dvec3 &v) {
        v.x = j.at(0).get<double>();
        v.y = j.at(1).get<double>();
        v.z = j.at(2).get<double>();
    }
};

template <> struct adl_serializer<glm::vec3> {
    static void to_json(json &j, const glm::vec3 &v) { j = {v.x, v.y, v.z}; }
    static void from_json(const json &j, glm::vec3 &v) {
        v.x = j.at(0).get<float>();
        v.y = j.at(1).get<float>();
        v.z = j.at(2).get<float>();
    }
};

template <> struct adl_serializer<glm::vec4> {
    static void to_json(json &j, const glm::vec4 &v) { j = {v.x, v.y, v.z, v.w}; }
    static void from_json(const json &j, glm::vec4 &v) {
        v.x = j.at(0).get<float>();
        v.y = j.at(1).get<float>();
        v.z = j.at(2).get<float>();
        v.w = j.at(3).get<float>();
    }
};

template <> struct adl_serializer<glm::dmat4> {
    static void to_json(json &j, const glm::dmat4 &m) {
        j = json::array();
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                j.push_back(m[col][row]);
    }
    static void from_json(const json &j, glm::dmat4 &m) {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] = j.at(static_cast<std::size_t>(col * 4 + row)).get<double>();
    }
};

template <> struct adl_serializer<glm::mat4> {
    static void to_json(json &j, const glm::mat4 &m) {
        j = json::array();
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                j.push_back(m[col][row]);
    }
    static void from_json(const json &j, glm::mat4 &m) {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m[col][row] = j.at(static_cast<std::size_t>(col * 4 + row)).get<float>();
    }
};

} // namespace nlohmann
