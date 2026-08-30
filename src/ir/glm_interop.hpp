#pragma once

// The one place the Semantic IR's own math meets glm.
//
// The IR stopped using glm so the amalgamated connector would not have to carry
// it (see ir/math.hpp). Everything downstream of the IR — the tessellator, the
// selector, the Render IR, the viewer — still uses glm and should: they do real
// geometry, and none of them is in the connector's closure.
//
// So this header is the seam, and it is deliberately explicit: `toGlm` and
// `fromGlm` at the call site rather than implicit conversions on the types
// themselves. An implicit conversion would make the boundary invisible, and the
// whole point is to be able to see, from a grep, which code pulls glm in.
//
// Conversions are element by element. The two layouts do match — both are
// column-major and tightly packed, and Mat4 static_asserts its own packing —
// but reinterpreting one as the other would be a silent assumption that passes
// every test right up until it does not.

#include <ir/math.hpp>

#include <glm/glm.hpp>

namespace nodehammer::ir {

inline glm::dmat4 toGlm(const Mat4 &m) {
    glm::dmat4 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c][r] = m[c][r];
        }
    }
    return out;
}

inline Mat4 fromGlm(const glm::dmat4 &m) {
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c][r] = m[c][r];
        }
    }
    return out;
}

inline glm::dvec3 toGlm(const Vec3 &v) { return {v.x, v.y, v.z}; }
inline Vec3 fromGlm(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }

inline glm::vec3 toGlm(const Color3 &c) { return {c.r, c.g, c.b}; }
inline Color3 colorFromGlm(const glm::vec3 &v) { return {v.x, v.y, v.z}; }

} // namespace nodehammer::ir
