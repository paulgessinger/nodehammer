#include <diagnostic_codes.hpp>
#include <ir/tgeo/semantic/matrix.hpp>
#include <ir/tgeo/semantic/shape_dispatch.hpp>

#include <TGeoBBox.h>
#include <TGeoBoolNode.h>
#include <TGeoCompositeShape.h>
#include <TGeoCone.h>
#include <TGeoMatrix.h>
#include <TGeoPcon.h>
#include <TGeoPgon.h>
#include <TGeoShape.h>
#include <TGeoTessellated.h>
#include <TGeoTorus.h>
#include <TGeoTrd1.h>
#include <TGeoTrd2.h>
#include <TGeoTube.h>

#include <format>

namespace nodehammer::ir {

semantic::ShapeId dispatchTGeoShape(const TGeoShape *shape, semantic::Scene &scene,
                                    DiagnosticList &diags) {
    semantic::ShapeVariant variant;

    // ── Composite (boolean) ───────────────────────────────────────────────────
    if (const auto *comp = dynamic_cast<const TGeoCompositeShape *>(shape)) {
        const TGeoBoolNode *bn = comp->GetBoolNode();
        const semantic::ShapeId leftId = dispatchTGeoShape(bn->GetLeftShape(), scene, diags);
        const semantic::ShapeId rightId = dispatchTGeoShape(bn->GetRightShape(), scene, diags);
        const glm::dmat4 rightTransform = tgeoMatrixToGlm(bn->GetRightMatrix());

        if (dynamic_cast<const TGeoUnion *>(bn) != nullptr) {
            variant = semantic::BooleanUnion{leftId, rightId, rightTransform};
        } else if (dynamic_cast<const TGeoSubtraction *>(bn) != nullptr) {
            variant = semantic::BooleanSubtraction{leftId, rightId, rightTransform};
        } else {
            variant = semantic::BooleanIntersection{leftId, rightId, rightTransform};
        }
    }
    // ── Tessellated ───────────────────────────────────────────────────────────
    else if (const auto *tess = dynamic_cast<const TGeoTessellated *>(shape)) {
        semantic::TessellatedShape ts;
        for (int i = 0; i < tess->GetNfacets(); ++i) {
            const TGeoFacet &f = tess->GetFacet(i);
            if (f.GetNvert() != 3) {
                continue; // skip non-triangle facets
            }
            semantic::TessellatedShape::Triangle tri{};
            for (int v = 0; v < 3; ++v) {
                const auto &pt = tess->GetVertex(f[v]);
                tri.vertices[static_cast<std::size_t>(v)] = glm::dvec3{pt.x(), pt.y(), pt.z()};
            }
            ts.triangles.push_back(tri);
        }
        variant = std::move(ts);
    }
    // ── Tube / TubeSeg ────────────────────────────────────────────────────────
    else if (const auto *tubeSeg = dynamic_cast<const TGeoTubeSeg *>(shape)) {
        variant = semantic::TubeShape{tubeSeg->GetRmin(), tubeSeg->GetRmax(), tubeSeg->GetDz(),
                                      tubeSeg->GetPhi1() * std::numbers::pi / 180.0,
                                      (tubeSeg->GetPhi2() - tubeSeg->GetPhi1()) * std::numbers::pi /
                                          180.0};
    } else if (const auto *tube = dynamic_cast<const TGeoTube *>(shape)) {
        variant = semantic::TubeShape{tube->GetRmin(), tube->GetRmax(), tube->GetDz()};
    }
    // ── Cone / ConeSeg ────────────────────────────────────────────────────────
    else if (const auto *coneSeg = dynamic_cast<const TGeoConeSeg *>(shape)) {
        variant = semantic::ConeShape{coneSeg->GetRmin1(),
                                      coneSeg->GetRmax1(),
                                      coneSeg->GetRmin2(),
                                      coneSeg->GetRmax2(),
                                      coneSeg->GetDz(),
                                      coneSeg->GetPhi1() * std::numbers::pi / 180.0,
                                      (coneSeg->GetPhi2() - coneSeg->GetPhi1()) * std::numbers::pi /
                                          180.0};
    } else if (const auto *cone = dynamic_cast<const TGeoCone *>(shape)) {
        variant = semantic::ConeShape{cone->GetRmin1(), cone->GetRmax1(), cone->GetRmin2(),
                                      cone->GetRmax2(), cone->GetDz()};
    }
    // ── Trd2 / Trd1 ───────────────────────────────────────────────────────────
    else if (const auto *trd2 = dynamic_cast<const TGeoTrd2 *>(shape)) {
        variant = semantic::TrdShape{trd2->GetDx1(), trd2->GetDx2(), trd2->GetDy1(), trd2->GetDy2(),
                                     trd2->GetDz()};
    } else if (const auto *trd1 = dynamic_cast<const TGeoTrd1 *>(shape)) {
        // TGeoTrd1: dy is constant on both ends
        variant = semantic::TrdShape{trd1->GetDx1(), trd1->GetDx2(), trd1->GetDy(), trd1->GetDy(),
                                     trd1->GetDz()};
    }
    // ── Torus ─────────────────────────────────────────────────────────────────
    else if (const auto *tor = dynamic_cast<const TGeoTorus *>(shape)) {
        variant = semantic::TorusShape{tor->GetRmin(), tor->GetRmax(), tor->GetR(),
                                       tor->GetPhi1() * std::numbers::pi / 180.0,
                                       tor->GetDphi() * std::numbers::pi / 180.0};
    }
    // ── Pgon (must precede Pcon — TGeoPgon derives from TGeoPcon) ────────────
    else if (const auto *pgon = dynamic_cast<const TGeoPgon *>(shape)) {
        semantic::PgonShape ps;
        ps.phiStart = pgon->GetPhi1() * std::numbers::pi / 180.0;
        ps.phiDelta = pgon->GetDphi() * std::numbers::pi / 180.0;
        ps.nSides = pgon->GetNedges();
        for (int i = 0; i < pgon->GetNz(); ++i) {
            ps.sections.push_back({pgon->GetZ(i), pgon->GetRmin(i), pgon->GetRmax(i)});
        }
        variant = std::move(ps);
    }
    // ── Pcon ──────────────────────────────────────────────────────────────────
    else if (const auto *pcon = dynamic_cast<const TGeoPcon *>(shape)) {
        semantic::PconShape ps;
        ps.phiStart = pcon->GetPhi1() * std::numbers::pi / 180.0;
        ps.phiDelta = pcon->GetDphi() * std::numbers::pi / 180.0;
        for (int i = 0; i < pcon->GetNz(); ++i) {
            ps.sections.push_back({pcon->GetZ(i), pcon->GetRmin(i), pcon->GetRmax(i)});
        }
        variant = std::move(ps);
    }
    // ── Box (must be last — it is the base class of most shapes) ─────────────
    else if (const auto *box = dynamic_cast<const TGeoBBox *>(shape)) {
        variant = semantic::BoxShape{box->GetDX(), box->GetDY(), box->GetDZ()};
    }
    // ── Unknown ───────────────────────────────────────────────────────────────
    else {
        const std::string typeName = shape->ClassName();
        diags.warn(codes::kWarnTgeoUnknownShape,
                   std::format("unknown TGeo shape type '{}'", typeName), shape->GetName());
        variant = semantic::UnknownShape{typeName};
    }

    const semantic::ShapeId id = scene.nextShapeId();
    scene.shapes[id] = semantic::Shape{id, std::move(variant)};
    return id;
}

} // namespace nodehammer::ir
