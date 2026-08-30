#pragma once

// nlohmann serializers for the Semantic IR's math types.
//
// Deliberately a separate header from ir/math.hpp, for the same reason
// ir/render_json.hpp is separate from ir/render.hpp: math.hpp exists to keep
// heavyweight dependencies out of the Semantic IR, and pulling nlohmann into it
// would trade one for another. Only the TUs that serialize include this.
//
// The emitted shape is identical to what detail/glm_json.hpp produced for the
// glm types these replaced — same keys, same element order, same
// identity/zero elision. `dump-semantic` output and the JSON importer are a
// format, and this change is not the place to alter one.

#include <ir/math.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>

namespace nlohmann {

template <> struct adl_serializer<nodehammer::ir::Vec3> {
    static void to_json(json &j, const nodehammer::ir::Vec3 &v) { j = {v.x, v.y, v.z}; }
    static void from_json(const json &j, nodehammer::ir::Vec3 &v) {
        v.x = j.at(0).get<double>();
        v.y = j.at(1).get<double>();
        v.z = j.at(2).get<double>();
    }
};

template <> struct adl_serializer<nodehammer::ir::Color3> {
    static void to_json(json &j, const nodehammer::ir::Color3 &c) { j = {c.r, c.g, c.b}; }
    static void from_json(const json &j, nodehammer::ir::Color3 &c) {
        c.r = j.at(0).get<float>();
        c.g = j.at(1).get<float>();
        c.b = j.at(2).get<float>();
    }
};

} // namespace nlohmann

// ── Flattened rotation/translation on a parent JSON object ───────────────────
// Usage: mat4ToJson(j, "locRot", "locTrl", m);  — writes keys directly on j
//        mat4FromJson(j, "locRot", "locTrl", m); — reads keys from j
//
// A transform is written as a 3×3 rotation and a 3-vector translation rather
// than sixteen numbers, and each half is omitted when it is the identity or
// zero. Most nodes in a real detector are one or the other.

inline void mat4ToJson(nlohmann::json &j, const char *rotKey, const char *trlKey,
                       const nodehammer::ir::Mat4 &m) {
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

inline void mat4FromJson(const nlohmann::json &j, const char *rotKey, const char *trlKey,
                         nodehammer::ir::Mat4 &m) {
    m = nodehammer::ir::Mat4{};
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
