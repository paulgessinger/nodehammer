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

} // namespace nlohmann

// ── Helpers for flattened rotation/translation on parent JSON objects ─────────
// Usage: dmat4ToJson(j, "locRot", "locTrl", m);  — writes keys directly on j
//        dmat4FromJson(j, "locRot", "locTrl", m); — reads keys from j

inline void dmat4ToJson(nlohmann::json &j, const char *rotKey, const char *trlKey,
                        const glm::dmat4 &m) {
    const bool identityRotation = m[0][0] == 1.0 && m[0][1] == 0.0 && m[0][2] == 0.0 &&
                                  m[1][0] == 0.0 && m[1][1] == 1.0 && m[1][2] == 0.0 &&
                                  m[2][0] == 0.0 && m[2][1] == 0.0 && m[2][2] == 1.0;
    const bool zeroTranslation = m[3][0] == 0.0 && m[3][1] == 0.0 && m[3][2] == 0.0;

    if (!identityRotation) {
        auto r = nlohmann::json::array();
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                r.push_back(m[col][row]);
            }
        }
        j[rotKey] = std::move(r);
    }
    if (!zeroTranslation) {
        j[trlKey] = {m[3][0], m[3][1], m[3][2]};
    }
}

inline void dmat4FromJson(const nlohmann::json &j, const char *rotKey, const char *trlKey,
                          glm::dmat4 &m) {
    m = glm::dmat4{1.0};
    if (j.contains(rotKey)) {
        const auto &r = j.at(rotKey);
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                m[col][row] = r.at(static_cast<std::size_t>(col * 3 + row)).get<double>();
            }
        }
    }
    if (j.contains(trlKey)) {
        const auto &t = j.at(trlKey);
        m[3][0] = t.at(0).get<double>();
        m[3][1] = t.at(1).get<double>();
        m[3][2] = t.at(2).get<double>();
    }
}

namespace nlohmann {

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
