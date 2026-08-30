#pragma once

// The small vector and matrix vocabulary the Semantic IR is built from.
//
// This exists so the Semantic IR does not depend on glm. That matters for one
// reason: the amalgamated connector (docs/event-display-design.md §7) inlines
// every dependency it cannot leave as a real `#include`, and glm was ~41% of the
// generated header — 27,795 lines of templates, precision variants and SIMD
// paths, to hold transforms that only ever get built, multiplied once and
// serialized.
//
// Narrowing the include does not help: `<glm/mat4x4.hpp>` pulls the same
// `detail/` and `simd/` machinery, and swapping it in changes the generated
// header by exactly zero lines. Measured, not assumed. The only way out is to
// not use glm's types here.
//
// Two things this is deliberately *not*:
//
//   * Not a general math library. The Render IR, the tessellator, the selector
//     and the viewer keep using glm, and should — they do real geometry, and
//     glm is better at it than anything worth writing here. Conversions live in
//     ir/glm_interop.hpp and are explicit at the boundary.
//   * Not layout-compatible with glm by contract. It happens to match (both are
//     column-major, tightly packed), and `ir/glm_interop.hpp` still converts
//     element by element rather than reinterpreting, because a silent layout
//     assumption is the kind of thing that survives every test and then breaks
//     on one compiler.
//
// Column-major, matching glm and OpenGL: `m[c]` is a *column*, so `m[3]` is the
// translation and `m[c][r]` is row r of column c. Getting that backwards
// transposes every transform in the tree, so it is worth saying twice.

#include <array>
#include <cstddef>
#include <type_traits>

namespace nodehammer::ir {

/// Three doubles — a position, direction or extent in the Semantic IR.
struct Vec3 {
    double x{0};
    double y{0};
    double z{0};

    friend bool operator==(const Vec3 &, const Vec3 &) = default;
};

/// Four doubles — a column of a Mat4, or a homogeneous point.
struct Vec4 {
    double x{0};
    double y{0};
    double z{0};
    double w{0};

    /// Row access within a column. Bounds are the caller's to respect; every
    /// use here is a literal 0-3 in a fixed loop.
    double &operator[](int r) { return (&x)[r]; }
    const double &operator[](int r) const { return (&x)[r]; }

    friend bool operator==(const Vec4 &, const Vec4 &) = default;
};

/// Linear RGB in [0,1].
///
/// Its own type rather than a Vec3 because it is the only place the Semantic IR
/// carried a vector whose components are not spatial, and because glm's `.r/.g/.b`
/// spelling came from an anonymous union that -Wpedantic rejects. Naming the
/// members what they are costs nothing and reads better than `color->x`.
struct Color3 {
    float r{0};
    float g{0};
    float b{0};

    friend bool operator==(const Color3 &, const Color3 &) = default;
};

/// Column-major 4x4 of doubles. Default-constructs to the identity, which is
/// what every transform field in the Semantic IR wants as its initial value.
struct Mat4 {
    std::array<Vec4, 4> cols{Vec4{1, 0, 0, 0}, Vec4{0, 1, 0, 0}, Vec4{0, 0, 1, 0},
                             Vec4{0, 0, 0, 1}};

    Mat4() = default;

    /// From four columns, in the order they appear left to right.
    Mat4(const Vec4 &c0, const Vec4 &c1, const Vec4 &c2, const Vec4 &c3) : cols{c0, c1, c2, c3} {}

    Vec4 &operator[](int c) { return cols[static_cast<std::size_t>(c)]; }
    const Vec4 &operator[](int c) const { return cols[static_cast<std::size_t>(c)]; }

    friend bool operator==(const Mat4 &, const Mat4 &) = default;
};

/// Matrix product, with glm's convention: column j of the result is `a` applied
/// to column j of `b`, so `a * b` means "b first, then a" when composing
/// transforms parent-to-child.
inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
    Mat4 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a[k][r] * b[c][k];
            }
            out[c][r] = sum;
        }
    }
    return out;
}

// `matEqual` in semantic.cpp compares transforms with std::memcmp, and the .nhb
// codec bit_casts elements straight out. Both are only meaningful if the storage
// is exactly sixteen doubles with nothing in between.
static_assert(sizeof(Mat4) == 16 * sizeof(double), "Mat4 must be tightly packed");
static_assert(std::is_trivially_copyable_v<Mat4>);
static_assert(sizeof(Vec3) == 3 * sizeof(double), "Vec3 must be tightly packed");
static_assert(sizeof(Color3) == 3 * sizeof(float), "Color3 must be tightly packed");

} // namespace nodehammer::ir
