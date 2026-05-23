#include <nodehammer/detail/overloaded.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>
#include <nodehammer/tessellation/primitive_tessellator.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <numbers>

namespace nodehammer {

using detail::overloaded;

namespace {

constexpr float kPi = static_cast<float>(std::numbers::pi);

// Azimuthal sweep angle for index i in [0, segs] over a span of `dphi` from
// `phi0`. For a full 2π sweep the final index must land *exactly* on phi0 rather
// than phi0+2π: sin/cos(2π) evaluated in float is off by ~2.6e-5 (the float
// nearest 2π is not a multiple of it), which leaves the wrap-around vertices
// ~26 nm away from the start vertices. That tiny gap opens the φ seam, so the
// mesh has unmatched boundary edges and Manifold rejects it as non-manifold the
// moment it is used as a boolean operand (e.g. an angle-cut subtraction). Snap
// the wrap to phi0 so the seam closes exactly.
inline float sweepAngle(float phi0, float dphi, int i, int segs) {
    if (i >= segs && std::abs(dphi - 2.0f * kPi) < 1e-4f) {
        return phi0;
    }
    return phi0 + dphi * static_cast<float>(i) / static_cast<float>(segs);
}

// Poloidal angle for a full 2π loop (torus tube cross-section); same seam fix.
inline float loopAngle(int j, int n) {
    if (j >= n) {
        return 0.0f;
    }
    return 2.0f * kPi * static_cast<float>(j) / static_cast<float>(n);
}

// Append a quad as two triangles (a,b,c) + (a,c,d), winding CCW when viewed
// from the outside (i.e. along the direction opposite to the face normal).
//
//   d --- c
//   |  \  |
//   a --- b
//
// Triangle 1: a → b → c
// Triangle 2: a → c → d
void appendQuad(TessellationOutput &out, const Vertex &a, const Vertex &b, const Vertex &c,
                const Vertex &d) {
    const auto base = static_cast<uint32_t>(out.vertices.size());
    out.vertices.push_back(a);
    out.vertices.push_back(b);
    out.vertices.push_back(c);
    out.vertices.push_back(d);
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 2);
    out.indices.push_back(base + 3);
}

// Append a single triangle a → b → c (CCW when viewed from outside).
void appendTriangle(TessellationOutput &out, const Vertex &a, const Vertex &b, const Vertex &c) {
    const auto base = static_cast<uint32_t>(out.vertices.size());
    out.vertices.push_back(a);
    out.vertices.push_back(b);
    out.vertices.push_back(c);
    out.indices.push_back(base + 0);
    out.indices.push_back(base + 1);
    out.indices.push_back(base + 2);
}

// ── Box ───────────────────────────────────────────────────────────────────────
//
// Axis-aligned box centred at the origin. Half-lengths dx, dy, dz define the
// extent along each axis.
//
//         (-dx,+dy,+dz) ---- (+dx,+dy,+dz)
//              /|               /|
//             / |              / |
//   (-dx,+dy,-dz) -------- (+dx,+dy,-dz)
//            |  |             |  |
//            | (-dx,-dy,+dz)--|-(+dx,-dy,+dz)
//            | /              | /
//   (-dx,-dy,-dz) -------- (+dx,-dy,-dz)
//
// Six faces, each split into two triangles (one quad each). Each face has a
// constant outward normal: ±X, ±Y, or ±Z. All 24 vertices are distinct because
// they each carry a different normal; sharing vertices across faces would require
// averaged normals, which would produce incorrect shading on a hard-edged box.

TessellationOutput tessellateBox(const BoxShape &s) {
    TessellationOutput out;
    const float x = static_cast<float>(s.dx);
    const float y = static_cast<float>(s.dy);
    const float z = static_cast<float>(s.dz);

    // +X face (normal points right)
    appendQuad(out, {glm::vec3{x, -y, -z}, glm::vec3{1, 0, 0}},
               {glm::vec3{x, y, -z}, glm::vec3{1, 0, 0}}, {glm::vec3{x, y, z}, glm::vec3{1, 0, 0}},
               {glm::vec3{x, -y, z}, glm::vec3{1, 0, 0}});
    // -X face (normal points left)
    appendQuad(out, {glm::vec3{-x, y, -z}, glm::vec3{-1, 0, 0}},
               {glm::vec3{-x, -y, -z}, glm::vec3{-1, 0, 0}},
               {glm::vec3{-x, -y, z}, glm::vec3{-1, 0, 0}},
               {glm::vec3{-x, y, z}, glm::vec3{-1, 0, 0}});
    // +Y face (normal points forward)
    appendQuad(out, {glm::vec3{-x, y, -z}, glm::vec3{0, 1, 0}},
               {glm::vec3{-x, y, z}, glm::vec3{0, 1, 0}}, {glm::vec3{x, y, z}, glm::vec3{0, 1, 0}},
               {glm::vec3{x, y, -z}, glm::vec3{0, 1, 0}});
    // -Y face (normal points back)
    appendQuad(out, {glm::vec3{x, -y, -z}, glm::vec3{0, -1, 0}},
               {glm::vec3{x, -y, z}, glm::vec3{0, -1, 0}},
               {glm::vec3{-x, -y, z}, glm::vec3{0, -1, 0}},
               {glm::vec3{-x, -y, -z}, glm::vec3{0, -1, 0}});
    // +Z face (normal points up)
    appendQuad(out, {glm::vec3{-x, -y, z}, glm::vec3{0, 0, 1}},
               {glm::vec3{x, -y, z}, glm::vec3{0, 0, 1}}, {glm::vec3{x, y, z}, glm::vec3{0, 0, 1}},
               {glm::vec3{-x, y, z}, glm::vec3{0, 0, 1}});
    // -Z face (normal points down)
    appendQuad(out, {glm::vec3{x, -y, -z}, glm::vec3{0, 0, -1}},
               {glm::vec3{-x, -y, -z}, glm::vec3{0, 0, -1}},
               {glm::vec3{-x, y, -z}, glm::vec3{0, 0, -1}},
               {glm::vec3{x, y, -z}, glm::vec3{0, 0, -1}});
    return out;
}

// ── Tube ──────────────────────────────────────────────────────────────────────
//
// Cylindrical tube (or partial arc) centred on the Z axis. Cross-section view:
//
//          rMax
//         /
//   ------+------   ← +Z cap (z = +hz)
//   |  hollow   |
//   |  centre   |   rMin > 0 → hollow (annular cross-section)
//   |           |   rMin = 0 → solid  (filled circle)
//   ------+------   ← -Z cap (z = -hz)
//
// Side view of one wall segment (solid tube, rMin = 0):
//
//   z=+hz  *------*   ← outer wall top edge (rMax)
//          |      |
//   z=-hz  *------*   ← outer wall bottom edge (rMax)
//
// Components emitted:
//   1. Outer wall: `segs` quads running from -hz to +hz at rMax. Each quad
//      spans one angular step; normals point radially outward.
//   2. Inner wall (rMin > 0 only): `segs` quads at rMin with inward normals.
//   3. ±Z caps:
//        rMin = 0 → triangle fan from the axis (segs triangles per cap).
//        rMin > 0 → annular ring of quads (segs quads per cap).
//   4. Side walls (partial arc, !full only): two flat rectangular faces that
//      close the open ends of the arc at phi0 and phi0+dphi.

TessellationOutput tessellateTube(const TubeShape &s, const TessellationParams &p) {
    TessellationOutput out;
    const int segs = std::max(3, p.maxSegmentsCircle);
    const float rMin = static_cast<float>(s.rMin);
    const float rMax = static_cast<float>(s.rMax);
    const float hz = static_cast<float>(s.dz);
    const float phi0 = static_cast<float>(s.phiStart);
    const float dphi = static_cast<float>(s.phiDelta);
    // A tube is "full" when it sweeps a complete circle; partial tubes need
    // extra side-wall faces to close the open arc ends.
    const bool full = (dphi >= 2.0f * kPi - 1e-6f);
    auto angle = [&](int i) { return sweepAngle(phi0, dphi, i, segs); };

    // ── Outer wall ────────────────────────────────────────────────────────────
    // One quad per angular segment. The outward radial direction at angle a is
    // (cos a, sin a, 0), used directly as the normal.
    for (int i = 0; i < segs; ++i) {
        const float a0 = angle(i), a1 = angle(i + 1);
        const glm::vec3 n0{std::cos(a0), std::sin(a0), 0};
        const glm::vec3 n1{std::cos(a1), std::sin(a1), 0};
        appendQuad(out, {glm::vec3{rMax * n0.x, rMax * n0.y, -hz}, n0},
                   {glm::vec3{rMax * n1.x, rMax * n1.y, -hz}, n1},
                   {glm::vec3{rMax * n1.x, rMax * n1.y, hz}, n1},
                   {glm::vec3{rMax * n0.x, rMax * n0.y, hz}, n0});
    }

    // ── Inner wall (hollow tube only) ─────────────────────────────────────────
    // Same layout as the outer wall, but normals point inward (negated radial).
    if (rMin > 0.0f) {
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            const glm::vec3 n0{-std::cos(a0), -std::sin(a0), 0};
            const glm::vec3 n1{-std::cos(a1), -std::sin(a1), 0};
            appendQuad(out, {glm::vec3{rMin * std::cos(a0), rMin * std::sin(a0), hz}, n0},
                       {glm::vec3{rMin * std::cos(a1), rMin * std::sin(a1), hz}, n1},
                       {glm::vec3{rMin * std::cos(a1), rMin * std::sin(a1), -hz}, n1},
                       {glm::vec3{rMin * std::cos(a0), rMin * std::sin(a0), -hz}, n0});
        }
    }

    // ── ±Z caps ───────────────────────────────────────────────────────────────
    // Winding order is reversed between the two caps so that both normals point
    // outward (away from the centre of the tube along Z).
    for (int sign = -1; sign <= 1; sign += 2) {
        const glm::vec3 capN{0, 0, static_cast<float>(sign)};
        const float capZ = static_cast<float>(sign) * hz;
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            if (rMin > 0.0f) {
                // Annular ring: quad from inner radius to outer radius.
                if (sign > 0) {
                    appendQuad(out,
                               {glm::vec3{rMin * std::cos(a0), rMin * std::sin(a0), capZ}, capN},
                               {glm::vec3{rMax * std::cos(a0), rMax * std::sin(a0), capZ}, capN},
                               {glm::vec3{rMax * std::cos(a1), rMax * std::sin(a1), capZ}, capN},
                               {glm::vec3{rMin * std::cos(a1), rMin * std::sin(a1), capZ}, capN});
                } else {
                    appendQuad(out,
                               {glm::vec3{rMin * std::cos(a1), rMin * std::sin(a1), capZ}, capN},
                               {glm::vec3{rMax * std::cos(a1), rMax * std::sin(a1), capZ}, capN},
                               {glm::vec3{rMax * std::cos(a0), rMax * std::sin(a0), capZ}, capN},
                               {glm::vec3{rMin * std::cos(a0), rMin * std::sin(a0), capZ}, capN});
                }
            } else {
                // Solid disc: triangle fan from the axis centre.
                if (sign > 0) {
                    appendTriangle(
                        out, {glm::vec3{0, 0, capZ}, capN},
                        {glm::vec3{rMax * std::cos(a0), rMax * std::sin(a0), capZ}, capN},
                        {glm::vec3{rMax * std::cos(a1), rMax * std::sin(a1), capZ}, capN});
                } else {
                    appendTriangle(
                        out, {glm::vec3{0, 0, capZ}, capN},
                        {glm::vec3{rMax * std::cos(a1), rMax * std::sin(a1), capZ}, capN},
                        {glm::vec3{rMax * std::cos(a0), rMax * std::sin(a0), capZ}, capN});
                }
            }
        }
    }

    // ── Side caps (partial arc only) ──────────────────────────────────────────
    // A partial tube is like a slice of pie: the two cut edges need flat faces.
    // Each side cap is a rectangle (or two triangles for a solid tube) whose
    // normal points perpendicular to the cut plane, inward toward the arc centre.
    //
    //        phi0+dphi
    //          /
    //    -----/---       The two side-cap faces close the open pie-slice ends.
    //    |  /     |      Normal at phi0:      (-sin(phi0),  cos(phi0), 0)
    //    |/       |      Normal at phi0+dphi: ( sin(end),  -cos(end),  0)
    //    |---------
    //          phi0
    if (!full) {
        for (int side = 0; side < 2; ++side) {
            const float a = (side == 0) ? phi0 : phi0 + dphi;
            // Normal perpendicular to the cut plane, pointing inward.
            const glm::vec3 n = (side == 0) ? glm::vec3{-std::sin(a), std::cos(a), 0}
                                            : glm::vec3{std::sin(a), -std::cos(a), 0};
            const float ca = std::cos(a), sa = std::sin(a);
            if (rMin > 0.0f) {
                if (side == 0) {
                    appendQuad(out, {glm::vec3{rMax * ca, rMax * sa, -hz}, n},
                               {glm::vec3{rMin * ca, rMin * sa, -hz}, n},
                               {glm::vec3{rMin * ca, rMin * sa, hz}, n},
                               {glm::vec3{rMax * ca, rMax * sa, hz}, n});
                } else {
                    appendQuad(out, {glm::vec3{rMin * ca, rMin * sa, -hz}, n},
                               {glm::vec3{rMax * ca, rMax * sa, -hz}, n},
                               {glm::vec3{rMax * ca, rMax * sa, hz}, n},
                               {glm::vec3{rMin * ca, rMin * sa, hz}, n});
                }
            } else {
                // Solid tube: the side cap is a full rectangle from axis to rMax.
                // Split into two triangles because the two ends of the rectangle
                // lie at z=-hz and z=+hz (four distinct corners).
                if (side == 0) {
                    appendTriangle(out, {glm::vec3{0, 0, -hz}, n},
                                   {glm::vec3{rMax * ca, rMax * sa, hz}, n},
                                   {glm::vec3{rMax * ca, rMax * sa, -hz}, n});
                    appendTriangle(out, {glm::vec3{0, 0, -hz}, n}, {glm::vec3{0, 0, hz}, n},
                                   {glm::vec3{rMax * ca, rMax * sa, hz}, n});
                } else {
                    appendTriangle(out, {glm::vec3{0, 0, -hz}, n},
                                   {glm::vec3{rMax * ca, rMax * sa, -hz}, n},
                                   {glm::vec3{rMax * ca, rMax * sa, hz}, n});
                    appendTriangle(out, {glm::vec3{0, 0, -hz}, n},
                                   {glm::vec3{rMax * ca, rMax * sa, hz}, n},
                                   {glm::vec3{0, 0, hz}, n});
                }
            }
        }
    }
    return out;
}

// ── Cone ──────────────────────────────────────────────────────────────────────
//
// Conical frustum (truncated cone) or solid cone, centred on the Z axis.
// Each end can independently have an inner and outer radius, allowing hollow
// frustums. Side view (frustum example):
//
/*
 *        rMin2  rMax2
 *         |-----|         z = +hz (top)
 *        /       \
 *       /         \       <- outer slant wall
 *      /     ___   \
 *     /_____|   |___\     z = -hz (bottom)
 */
//
//      |-----|   |---|
//      rMin1     rMax1
//
// When rMax2 == 0 (solid cone with apex at top), the top face degenerates to a
// point. The outer wall emits one triangle per segment converging to (0,0,+hz),
// and the top cap is skipped entirely since it has zero area.
//
// Slant surface normal: the outward normal on the outer wall is perpendicular to
// the slant line in the r-z plane. For a slant from (rMax1, -hz) to (rMax2, +hz),
// the 2D slant vector is (dR, 2*hz) where dR = rMax2 - rMax1. The perpendicular
// (outward) is (2*hz, -dR), normalised. In 3D this is rotated around Z at each
// azimuthal angle.

TessellationOutput tessellateCone(const ConeShape &s, const TessellationParams &p) {
    TessellationOutput out;
    const int segs = std::max(3, p.maxSegmentsCircle);
    const float rMin1 = static_cast<float>(s.rMin1);
    const float rMax1 = static_cast<float>(s.rMax1);
    const float rMin2 = static_cast<float>(s.rMin2);
    const float rMax2 = static_cast<float>(s.rMax2);
    const float hz = static_cast<float>(s.dz);
    const float phi0 = static_cast<float>(s.phiStart);
    const float dphi = static_cast<float>(s.phiDelta);
    auto angle = [&](int i) { return sweepAngle(phi0, dphi, i, segs); };

    // Precompute the Z and radial components of the outer slant normal.
    // snz: Z component; snr: radial (r) component (same for all azimuthal angles).
    const float dR = rMax2 - rMax1;
    const float slantLen = std::sqrt(dR * dR + 4.0f * hz * hz);
    const float snz = (slantLen > 1e-12f) ? (2.0f * hz / slantLen) : 1.0f;
    const float snr = (slantLen > 1e-12f) ? (-dR / slantLen) : 0.0f;

    // ── Outer wall ────────────────────────────────────────────────────────────
    // Frustum: one quad per segment. Cone apex: one triangle per segment.
    for (int i = 0; i < segs; ++i) {
        const float a0 = angle(i), a1 = angle(i + 1);
        const glm::vec3 n0{snr * std::cos(a0), snr * std::sin(a0), snz};
        const glm::vec3 n1{snr * std::cos(a1), snr * std::sin(a1), snz};
        const glm::vec3 topL{rMax2 * std::cos(a0), rMax2 * std::sin(a0), hz};
        const glm::vec3 topR{rMax2 * std::cos(a1), rMax2 * std::sin(a1), hz};
        const glm::vec3 botL{rMax1 * std::cos(a0), rMax1 * std::sin(a0), -hz};
        const glm::vec3 botR{rMax1 * std::cos(a1), rMax1 * std::sin(a1), -hz};
        if (rMax2 < 1e-9f) {
            // Apex at top: wall segment collapses to a triangle.
            appendTriangle(out, {botL, n0}, {botR, n1}, {glm::vec3{0, 0, hz}, glm::vec3{0, 0, 1}});
        } else if (rMax1 < 1e-9f) {
            // Apex at bottom: wall segment collapses to a triangle.
            appendTriangle(out, {glm::vec3{0, 0, -hz}, glm::vec3{0, 0, -1}}, {topR, n1},
                           {topL, n0});
        } else {
            appendQuad(out, {botL, n0}, {botR, n1}, {topR, n1}, {topL, n0});
        }
    }

    // ── Inner wall (hollow frustum only) ──────────────────────────────────────
    // Present when either end has a non-zero inner radius. Normals point inward.
    if (rMin1 > 0.0f || rMin2 > 0.0f) {
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            const glm::vec3 n0{-std::cos(a0), -std::sin(a0), 0};
            const glm::vec3 n1{-std::cos(a1), -std::sin(a1), 0};
            appendQuad(out, {glm::vec3{rMin2 * std::cos(a0), rMin2 * std::sin(a0), hz}, n0},
                       {glm::vec3{rMin2 * std::cos(a1), rMin2 * std::sin(a1), hz}, n1},
                       {glm::vec3{rMin1 * std::cos(a1), rMin1 * std::sin(a1), -hz}, n1},
                       {glm::vec3{rMin1 * std::cos(a0), rMin1 * std::sin(a0), -hz}, n0});
        }
    }

    // ── Caps ──────────────────────────────────────────────────────────────────
    // Skip if the outer radius is zero at this end.
    // When rOuter == 0, all cap vertices would collapse to a single point,
    // producing zero-area (degenerate) triangles. The outer wall already closes
    // the surface at the apex: each wall triangle converges to (0, 0, capZ),
    // so no cap is needed.
    for (int sign = -1; sign <= 1; sign += 2) {
        const float capZ = static_cast<float>(sign) * hz;
        const float rOuter = (sign > 0) ? rMax2 : rMax1;
        const float rInner = (sign > 0) ? rMin2 : rMin1;
        if (rOuter < 1e-9f) {
            continue;
        }
        const glm::vec3 capN{0, 0, static_cast<float>(sign)};
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            if (rInner > 0.0f) {
                if (sign > 0) {
                    appendQuad(
                        out, {glm::vec3{rInner * std::cos(a0), rInner * std::sin(a0), capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a0), rOuter * std::sin(a0), capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a1), rOuter * std::sin(a1), capZ}, capN},
                        {glm::vec3{rInner * std::cos(a1), rInner * std::sin(a1), capZ}, capN});
                } else {
                    appendQuad(
                        out, {glm::vec3{rInner * std::cos(a1), rInner * std::sin(a1), capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a1), rOuter * std::sin(a1), capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a0), rOuter * std::sin(a0), capZ}, capN},
                        {glm::vec3{rInner * std::cos(a0), rInner * std::sin(a0), capZ}, capN});
                }
            } else {
                if (sign > 0) {
                    appendTriangle(
                        out, {glm::vec3{0, 0, capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a0), rOuter * std::sin(a0), capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a1), rOuter * std::sin(a1), capZ}, capN});
                } else {
                    appendTriangle(
                        out, {glm::vec3{0, 0, capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a1), rOuter * std::sin(a1), capZ}, capN},
                        {glm::vec3{rOuter * std::cos(a0), rOuter * std::sin(a0), capZ}, capN});
                }
            }
        }
    }
    return out;
}

// ── Trd ───────────────────────────────────────────────────────────────────────
//
// Trapezoid solid with a rectangular cross-section at each Z face. The top and
// bottom rectangles can have different half-widths (dx1/dy1 at -z, dx2/dy2 at +z),
// making the four side faces trapezoidal. Special case: if one end has zero
// dimensions it becomes a wedge.
//
// Corner labelling (b = bottom at -dz, t = top at +dz, 0–3 = CCW from -x,-y):
//
//   t3(-x2,+y2,+z) ---- t2(+x2,+y2,+z)       z = +dz
//       |                    |
//   t0(-x2,-y2,+z) ---- t1(+x2,-y2,+z)
//
//   b3(-x1,+y1,-z) ---- b2(+x1,+y1,-z)       z = -dz
//       |                    |
//   b0(-x1,-y1,-z) ---- b1(+x1,-y1,-z)
//
// Face normals are computed from the cross product of two edges in that face,
// so they correctly follow the slant of each trapezoidal side.

TessellationOutput tessellateTrd(const TrdShape &s) {
    TessellationOutput out;
    const float x1 = static_cast<float>(s.dx1);
    const float x2 = static_cast<float>(s.dx2);
    const float y1 = static_cast<float>(s.dy1);
    const float y2 = static_cast<float>(s.dy2);
    const float z = static_cast<float>(s.dz);

    const glm::vec3 b0{-x1, -y1, -z};
    const glm::vec3 b1{x1, -y1, -z};
    const glm::vec3 b2{x1, y1, -z};
    const glm::vec3 b3{-x1, y1, -z};
    const glm::vec3 t0{-x2, -y2, z};
    const glm::vec3 t1{x2, -y2, z};
    const glm::vec3 t2{x2, y2, z};
    const glm::vec3 t3{-x2, y2, z};

    // Face winding follows the same convention as tessellateBox:
    // each face's vertices go CCW when viewed from outside (outward normal).
    auto fn = [](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        return glm::normalize(glm::cross(b - a, c - a));
    };

    auto nxp = fn(b1, b2, t2);
    appendQuad(out, {b1, nxp}, {b2, nxp}, {t2, nxp}, {t1, nxp}); // +X
    auto nxn = fn(b3, b0, t0);
    appendQuad(out, {b3, nxn}, {b0, nxn}, {t0, nxn}, {t3, nxn}); // -X  (was: b3,t3,t0,b0)
    auto nyp = fn(b2, b3, t3);
    appendQuad(out, {b2, nyp}, {b3, nyp}, {t3, nyp}, {t2, nyp}); // +Y  (was: b3,b2,t2,t3)
    auto nyn = fn(b0, b1, t1);
    appendQuad(out, {b0, nyn}, {b1, nyn}, {t1, nyn}, {t0, nyn}); // -Y  (was: b0,t0,t1,b1)
    appendQuad(out, {t0, {0, 0, 1}}, {t1, {0, 0, 1}}, {t2, {0, 0, 1}}, {t3, {0, 0, 1}});     // +Z
    appendQuad(out, {b1, {0, 0, -1}}, {b0, {0, 0, -1}}, {b3, {0, 0, -1}}, {b2, {0, 0, -1}}); // -Z
    return out;
}

// ── Para ──────────────────────────────────────────────────────────────────────
//
// Parallelepiped: a box sheared by angles alpha, theta, phi. Geant4/ROOT
// convention:
//   - alpha: shear of the Y faces in X (face at +y slides by alpha*dy in X).
//   - theta, phi: polar and azimuthal tilt of the Z axis. The Z faces slide by
//     tan(theta)*cos(phi)*dz in X and tan(theta)*sin(phi)*dz in Y per unit dz.
//
// The shape still has six flat faces and eight corners, just like a box, but
// none of the faces need be perpendicular to the axes.
//
//         t3 ---- t2       ← z = +dz face (shifted by dzShift)
//        /|      /|
//       t0 ---- t1 |
//       | b3 ---|- b2      ← z = -dz face (shifted by -dzShift)
//       |/      |/
//       b0 ---- b1
//
// Face normals are derived from the cross product of edge vectors, same as Trd.

TessellationOutput tessellatePara(const ParaShape &s) {
    TessellationOutput out;
    const float dx = static_cast<float>(s.dx);
    const float dy = static_cast<float>(s.dy);
    const float dz = static_cast<float>(s.dz);
    const float ta = std::tan(static_cast<float>(s.alpha));
    const float tth = std::tan(static_cast<float>(s.theta));
    const float cphi = std::cos(static_cast<float>(s.phi));
    const float sphi = std::sin(static_cast<float>(s.phi));

    // dzShift: offset applied to top (+dz) and bottom (-dz) faces due to
    // theta/phi tilt. The bottom face shifts by -dzShift, the top by +dzShift.
    const glm::vec3 dzShift{tth * cphi * dz, tth * sphi * dz, 0.0f};
    // dyShift: offset in X per unit Y, due to alpha shear.
    const glm::vec3 dyShift{ta * dy, 0.0f, 0.0f};

    const glm::vec3 b0 = -dzShift + glm::vec3{-dx - dyShift.x, -dy, -dz};
    const glm::vec3 b1 = -dzShift + glm::vec3{dx - dyShift.x, -dy, -dz};
    const glm::vec3 b2 = -dzShift + glm::vec3{dx + dyShift.x, dy, -dz};
    const glm::vec3 b3 = -dzShift + glm::vec3{-dx + dyShift.x, dy, -dz};
    const glm::vec3 t0 = dzShift + glm::vec3{-dx - dyShift.x, -dy, dz};
    const glm::vec3 t1 = dzShift + glm::vec3{dx - dyShift.x, -dy, dz};
    const glm::vec3 t2 = dzShift + glm::vec3{dx + dyShift.x, dy, dz};
    const glm::vec3 t3 = dzShift + glm::vec3{-dx + dyShift.x, dy, dz};

    auto fn = [](glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        return glm::normalize(glm::cross(b - a, c - a));
    };
    auto nxp = fn(b1, t1, t2);
    appendQuad(out, {b1, nxp}, {t1, nxp}, {t2, nxp}, {b2, nxp}); // +X
    auto nxn = fn(b3, t3, t0);
    appendQuad(out, {b3, nxn}, {t3, nxn}, {t0, nxn}, {b0, nxn}); // -X
    auto nyp = fn(b3, b2, t2);
    appendQuad(out, {b3, nyp}, {b2, nyp}, {t2, nyp}, {t3, nyp}); // +Y
    auto nyn = fn(b0, t0, t1);
    appendQuad(out, {b0, nyn}, {t0, nyn}, {t1, nyn}, {b1, nyn}); // -Y
    auto ntp = fn(t0, t1, t2);
    appendQuad(out, {t0, ntp}, {t1, ntp}, {t2, ntp}, {t3, ntp}); // +Z
    auto ntn = fn(b3, b2, b1);
    appendQuad(out, {b3, ntn}, {b2, ntn}, {b1, ntn}, {b0, ntn}); // -Z
    return out;
}

// ── Torus ─────────────────────────────────────────────────────────────────────
//
// Torus (donut) centred at the origin, axis along Z. Defined by:
//   rTor — major radius: distance from the Z axis to the tube centre.
//   rMax — outer radius of the tube cross-section, measured from the tube centre.
//   rMin — inner radius of the tube cross-section, measured from the tube centre.
//          rMin = 0 → solid tube (filled disc cross-section).
//          rMin > 0 → hollow tube (annular cross-section, like a bent pipe).
//
// Parameterisation: two angles phi (toroidal, sweeping around Z) and theta
// (poloidal, sweeping around the tube centre).
//
//    Z axis
//      |
//      |<----rTor---->o<--rMin-->|<---(rMax-rMin)--->|
//                    ↑          ↑                    ↑
//               tube centre  inner wall           outer wall
//
// Position of a point on the outer surface (rMax):
//   x = (rTor + rMax * cos θ) * cos φ
//   y = (rTor + rMax * cos θ) * sin φ
//   z =  rMax * sin θ
//
// The outward normal on the outer surface is the radial direction from the tube
// centre: (cos θ * cos φ,  cos θ * sin φ,  sin θ). On the inner surface the
// normal is negated.
//
// The double loop produces `segs * nTube` quads per surface. For a hollow torus
// two passes are made (outer then inner). nTube = segs/2 keeps poloidal
// resolution roughly proportional to toroidal resolution.

TessellationOutput tessellateTorus(const TorusShape &s, const TessellationParams &p) {
    TessellationOutput out;
    const int segs = std::max(3, p.maxSegmentsCircle);
    const int nTube = std::max(3, segs / 2); // poloidal segments around the tube
    const float rTor = static_cast<float>(s.rTor);
    const float rOuter = static_cast<float>(s.rMax); // tube outer radius from tube centre
    const float rInner = static_cast<float>(s.rMin); // tube inner radius from tube centre
    const float phi0 = static_cast<float>(s.phiStart);
    const float dphi = static_cast<float>(s.phiDelta);

    // Compute a surface vertex at toroidal index i, poloidal index j, on the
    // surface at tube radius r. normalSign: +1 for outward (outer surface),
    // -1 for inward (inner surface).
    auto vert = [&](int i, int j, float r, float normalSign) -> Vertex {
        const float phi = sweepAngle(phi0, dphi, i, segs);
        const float theta = loopAngle(j, nTube);
        const float ct = std::cos(theta), st = std::sin(theta);
        const float cp = std::cos(phi), sp = std::sin(phi);
        const glm::vec3 pos{(rTor + r * ct) * cp, (rTor + r * ct) * sp, r * st};
        const glm::vec3 n = normalSign * glm::normalize(glm::vec3{ct * cp, ct * sp, st});
        return {pos, n};
    };

    // Outer surface.
    for (int i = 0; i < segs; ++i) {
        for (int j = 0; j < nTube; ++j) {
            appendQuad(out, vert(i, j, rOuter, +1.0f), vert(i + 1, j, rOuter, +1.0f),
                       vert(i + 1, j + 1, rOuter, +1.0f), vert(i, j + 1, rOuter, +1.0f));
        }
    }

    // Inner surface (hollow torus only). Winding is reversed so the normal
    // faces inward (toward the tube centre).
    if (rInner > 0.0f) {
        for (int i = 0; i < segs; ++i) {
            for (int j = 0; j < nTube; ++j) {
                appendQuad(out, vert(i, j + 1, rInner, -1.0f), vert(i + 1, j + 1, rInner, -1.0f),
                           vert(i + 1, j, rInner, -1.0f), vert(i, j, rInner, -1.0f));
            }
        }
    }
    return out;
}

// ── Pcon ──────────────────────────────────────────────────────────────────────
//
// Polycone: a solid of revolution built from a sequence of coaxial annular
// sections stacked along Z. Each section is defined by (z, rMin, rMax). Between
// adjacent sections the outer and inner radii can vary, producing a stepped or
// tapered profile.
//
// Cross-section (one side of Z axis), three sections example:
//
//   rMax  ___
//        |   |___
//   rMin  |___|   |___     ← inner wall only emitted where rMin > 0
//         z0  z1  z2  z3
//
// Each consecutive pair of sections produces:
//   - One outer wall strip (segs quads), normals pointing radially outward.
//   - One inner wall strip (segs quads) if either end has rMin > 0,
//     normals pointing radially inward.
//
// End caps are emitted at the first and last section (bottom and top of the
// stack). Each cap is either a filled circle fan (rMin ≈ 0) or an annular ring
// (rMin > 0). The cap normal is (0,0,-1) for the bottom and (0,0,+1) for the
// top.

TessellationOutput tessellatePcon(const PconShape &s, const TessellationParams &p) {
    TessellationOutput out;
    if (s.sections.size() < 2) {
        return out;
    }
    const int segs = std::max(3, p.maxSegmentsCircle);
    const float phi0 = static_cast<float>(s.phiStart);
    const float dphi = static_cast<float>(s.phiDelta);
    auto angle = [&](int i) { return sweepAngle(phi0, dphi, i, segs); };

    // ── Walls (per section pair) ─────────────────────────────────────────────
    for (std::size_t k = 0; k + 1 < s.sections.size(); ++k) {
        const float z0 = static_cast<float>(s.sections[k].z);
        const float z1 = static_cast<float>(s.sections[k + 1].z);
        const float oBot = static_cast<float>(s.sections[k].rMax);
        const float oTop = static_cast<float>(s.sections[k + 1].rMax);
        const float iBot = static_cast<float>(s.sections[k].rMin);
        const float iTop = static_cast<float>(s.sections[k + 1].rMin);
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            const float ca0 = std::cos(a0), sa0 = std::sin(a0);
            const float ca1 = std::cos(a1), sa1 = std::sin(a1);
            // Outer wall — normal approximated as radially outward (ignores slant).
            appendQuad(out, {glm::vec3{oBot * ca0, oBot * sa0, z0}, glm::vec3{ca0, sa0, 0}},
                       {glm::vec3{oBot * ca1, oBot * sa1, z0}, glm::vec3{ca1, sa1, 0}},
                       {glm::vec3{oTop * ca1, oTop * sa1, z1}, glm::vec3{ca1, sa1, 0}},
                       {glm::vec3{oTop * ca0, oTop * sa0, z1}, glm::vec3{ca0, sa0, 0}});
            // Inner wall: skip when there is no surface area to emit. When
            // z0==z1 and iBot==iTop the "wall" degenerates to a circle (zero
            // area). The outer wall already handles the radial ring in that case.
            const bool innerHasArea = (iBot > 0.0f || iTop > 0.0f) &&
                                      !(std::abs(z1 - z0) < 1e-9f && std::abs(iTop - iBot) < 1e-9f);
            if (innerHasArea) {
                appendQuad(out, {glm::vec3{iTop * ca0, iTop * sa0, z1}, glm::vec3{-ca0, -sa0, 0}},
                           {glm::vec3{iTop * ca1, iTop * sa1, z1}, glm::vec3{-ca1, -sa1, 0}},
                           {glm::vec3{iBot * ca1, iBot * sa1, z0}, glm::vec3{-ca1, -sa1, 0}},
                           {glm::vec3{iBot * ca0, iBot * sa0, z0}, glm::vec3{-ca0, -sa0, 0}});
            }
        }
    }

    // ── End caps ─────────────────────────────────────────────────────────────
    // sign=-1 → bottom cap at z=sections.front().z, normal (0,0,-1)
    // sign=+1 → top cap    at z=sections.back().z,  normal (0,0,+1)
    for (int sign = -1; sign <= 1; sign += 2) {
        const auto &sec = (sign < 0) ? s.sections.front() : s.sections.back();
        const float capZ = static_cast<float>(sec.z);
        const float rOuter = static_cast<float>(sec.rMax);
        const float rInner = static_cast<float>(sec.rMin);
        if (rOuter < 1e-9f) {
            continue;
        }
        const glm::vec3 capN{0, 0, static_cast<float>(sign)};
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            const float ca0 = std::cos(a0), sa0 = std::sin(a0);
            const float ca1 = std::cos(a1), sa1 = std::sin(a1);
            if (rInner > 0.0f) {
                // Hollow: annular ring quad.
                if (sign > 0) {
                    appendQuad(out, {glm::vec3{rInner * ca0, rInner * sa0, capZ}, capN},
                               {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN},
                               {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN},
                               {glm::vec3{rInner * ca1, rInner * sa1, capZ}, capN});
                } else {
                    appendQuad(out, {glm::vec3{rInner * ca1, rInner * sa1, capZ}, capN},
                               {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN},
                               {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN},
                               {glm::vec3{rInner * ca0, rInner * sa0, capZ}, capN});
                }
            } else {
                // Solid: triangle fan from the axis.
                if (sign > 0) {
                    appendTriangle(out, {glm::vec3{0, 0, capZ}, capN},
                                   {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN},
                                   {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN});
                } else {
                    appendTriangle(out, {glm::vec3{0, 0, capZ}, capN},
                                   {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN},
                                   {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN});
                }
            }
        }
    }

    return out;
}

// ── Pgon ──────────────────────────────────────────────────────────────────────
//
// Polyhedron of revolution: like Pcon, but the cross-section is a regular
// polygon (nSides flat faces) rather than a smooth circle. The number of angular
// segments is fixed to nSides (ignoring maxSegmentsCircle), since each side of
// the polygon is a flat face with a single shared normal.
//
// ROOT (and Geant4) define rMax as the *apothem* — the perpendicular distance
// from the Z axis to the midpoint of a face. The polygon vertices lie at the
// circumradius = rMax / cos(π/nSides):
//
//   Top view (nSides = 6):
//
/*
 *      V-------V      <- vertices at circumradius = rMax / cos(pi/6)
 *     / |     | \
 *    /  |<rMax>|  \   <- rMax = apothem (face midpoint distance)
 *    \  |     |  /
 *     \ |     | /
 *      V-------V
 */
//
// The face normal for each side points from the axis to the face midpoint, i.e.
// in the direction at the midpoint angle amid = (a0+a1)/2.
//
// End caps are emitted at the first and last section, using the same winding
// convention as Pcon (solid polygon fan or annular polygon ring).

TessellationOutput tessellatePgon(const PgonShape &s, const TessellationParams &p) {
    TessellationOutput out;
    if (s.sections.size() < 2 || s.nSides < 3) {
        return out;
    }
    const int segs = s.nSides; // one segment per polygon face
    const float phi0 = static_cast<float>(s.phiStart);
    const float dphi = static_cast<float>(s.phiDelta);
    auto angle = [&](int i) { return sweepAngle(phi0, dphi, i, segs); };
    // Convert apothem → circumradius. Each polygon face spans dphi/nSides
    // radians; the half-angle of that sector gives the apothem/circumradius ratio.
    const float cosPN = std::cos(dphi / (2.0f * static_cast<float>(segs)));
    auto circumR = [cosPN](float apothem) -> float {
        return (cosPN > 1e-9f) ? apothem / cosPN : apothem;
    };
    (void)p;

    // ── Walls (per section pair) ─────────────────────────────────────────────
    for (std::size_t k = 0; k + 1 < s.sections.size(); ++k) {
        const float z0 = static_cast<float>(s.sections[k].z);
        const float z1 = static_cast<float>(s.sections[k + 1].z);
        const float oBot = circumR(static_cast<float>(s.sections[k].rMax));
        const float oTop = circumR(static_cast<float>(s.sections[k + 1].rMax));
        const float iBot =
            (s.sections[k].rMin > 0) ? circumR(static_cast<float>(s.sections[k].rMin)) : 0.0f;
        const float iTop = (s.sections[k + 1].rMin > 0)
                               ? circumR(static_cast<float>(s.sections[k + 1].rMin))
                               : 0.0f;
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            // Outward normal perpendicular to this flat face.
            const float amid = (a0 + a1) * 0.5f;
            const glm::vec3 nOut{std::cos(amid), std::sin(amid), 0};
            appendQuad(out, {glm::vec3{oBot * std::cos(a0), oBot * std::sin(a0), z0}, nOut},
                       {glm::vec3{oBot * std::cos(a1), oBot * std::sin(a1), z0}, nOut},
                       {glm::vec3{oTop * std::cos(a1), oTop * std::sin(a1), z1}, nOut},
                       {glm::vec3{oTop * std::cos(a0), oTop * std::sin(a0), z1}, nOut});
            if (iBot > 0.0f || iTop > 0.0f) {
                // Inner wall — inward normal, reversed Z order for correct winding.
                const glm::vec3 nIn{-std::cos(amid), -std::sin(amid), 0};
                appendQuad(out, {glm::vec3{iTop * std::cos(a0), iTop * std::sin(a0), z1}, nIn},
                           {glm::vec3{iTop * std::cos(a1), iTop * std::sin(a1), z1}, nIn},
                           {glm::vec3{iBot * std::cos(a1), iBot * std::sin(a1), z0}, nIn},
                           {glm::vec3{iBot * std::cos(a0), iBot * std::sin(a0), z0}, nIn});
            }
        }
    }

    // ── End caps ─────────────────────────────────────────────────────────────
    // Same winding convention as Pcon caps.
    for (int sign = -1; sign <= 1; sign += 2) {
        const auto &sec = (sign < 0) ? s.sections.front() : s.sections.back();
        const float capZ = static_cast<float>(sec.z);
        const float rOuter = circumR(static_cast<float>(sec.rMax));
        const float rInner = (sec.rMin > 0) ? circumR(static_cast<float>(sec.rMin)) : 0.0f;
        if (rOuter < 1e-9f) {
            continue;
        }
        const glm::vec3 capN{0, 0, static_cast<float>(sign)};
        for (int i = 0; i < segs; ++i) {
            const float a0 = angle(i), a1 = angle(i + 1);
            const float ca0 = std::cos(a0), sa0 = std::sin(a0);
            const float ca1 = std::cos(a1), sa1 = std::sin(a1);
            if (rInner > 0.0f) {
                // Hollow: annular polygon ring.
                if (sign > 0) {
                    appendQuad(out, {glm::vec3{rInner * ca0, rInner * sa0, capZ}, capN},
                               {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN},
                               {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN},
                               {glm::vec3{rInner * ca1, rInner * sa1, capZ}, capN});
                } else {
                    appendQuad(out, {glm::vec3{rInner * ca1, rInner * sa1, capZ}, capN},
                               {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN},
                               {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN},
                               {glm::vec3{rInner * ca0, rInner * sa0, capZ}, capN});
                }
            } else {
                // Solid: polygon fan from the axis.
                if (sign > 0) {
                    appendTriangle(out, {glm::vec3{0, 0, capZ}, capN},
                                   {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN},
                                   {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN});
                } else {
                    appendTriangle(out, {glm::vec3{0, 0, capZ}, capN},
                                   {glm::vec3{rOuter * ca1, rOuter * sa1, capZ}, capN},
                                   {glm::vec3{rOuter * ca0, rOuter * sa0, capZ}, capN});
                }
            }
        }
    }

    return out;
}

// ── TessellatedShape ──────────────────────────────────────────────────────────
//
// The shape already carries explicit triangle soup (e.g. from a TGeoTessellated
// or G4TessellatedSolid). Each triangle is emitted as-is, with a flat face
// normal computed from the cross product of its two edge vectors. Vertices are
// not shared or welded — each triangle owns three independent vertices, which
// is consistent with the flat-shading convention used by the other tessellators.

TessellationOutput tessellateTessellated(const TessellatedShape &s) {
    TessellationOutput out;
    out.vertices.reserve(s.triangles.size() * 3);
    out.indices.reserve(s.triangles.size() * 3);
    for (const auto &tri : s.triangles) {
        const glm::vec3 v0 = glm::vec3(tri.vertices[0]);
        const glm::vec3 v1 = glm::vec3(tri.vertices[1]);
        const glm::vec3 v2 = glm::vec3(tri.vertices[2]);
        const glm::vec3 n = glm::normalize(glm::cross(v1 - v0, v2 - v0));
        appendTriangle(out, {v0, n}, {v1, n}, {v2, n});
    }
    return out;
}

} // namespace

// ── PrimitiveTessellator ──────────────────────────────────────────────────────

bool PrimitiveTessellator::canTessellate(const SemanticShapeVariant &shape) const noexcept {
    return !std::holds_alternative<BooleanUnion>(shape) &&
           !std::holds_alternative<BooleanIntersection>(shape) &&
           !std::holds_alternative<BooleanSubtraction>(shape);
}

TessellationOutput PrimitiveTessellator::tessellate(const SemanticShapeVariant &shape,
                                                    const TessellationParams &params) const {
    return std::visit(
        overloaded{
            [&](const BoxShape &s) { return tessellateBox(s); },
            [&](const TubeShape &s) { return tessellateTube(s, params); },
            [&](const ConeShape &s) { return tessellateCone(s, params); },
            [&](const TrdShape &s) { return tessellateTrd(s); },
            [&](const ParaShape &s) { return tessellatePara(s); },
            [&](const TorusShape &s) -> TessellationOutput { return tessellateTorus(s, params); },
            [&](const PconShape &s) { return tessellatePcon(s, params); },
            [&](const PgonShape &s) { return tessellatePgon(s, params); },
            [&](const TessellatedShape &s) { return tessellateTessellated(s); },
            [](const UnknownShape &s) {
                TessellationOutput err;
                err.diags.error(codes::kErrTessUnknownShape,
                                "cannot tessellate unknown shape: " + s.originalType,
                                s.originalType);
                return err;
            },
            [](const auto &) {
                // Boolean — caller should have checked canTessellate()
                TessellationOutput err;
                err.diags.error(codes::kErrTessUnknownShape,
                                "boolean shape passed to PrimitiveTessellator", "boolean");
                return err;
            },
        },
        shape);
}

} // namespace nodehammer
