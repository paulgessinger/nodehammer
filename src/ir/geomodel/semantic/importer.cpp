#include <GeoModelDBManager/GMDBManager.h>
#include <GeoModelKernel/GeoBox.h>
#include <GeoModelKernel/GeoCons.h>
#include <GeoModelKernel/GeoGenericTrap.h>
#include <GeoModelKernel/GeoMaterial.h>
#include <GeoModelKernel/GeoPara.h>
#include <GeoModelKernel/GeoPcon.h>
#include <GeoModelKernel/GeoPgon.h>
#include <GeoModelKernel/GeoPolyhedrizeAction.h>
#include <GeoModelKernel/GeoPolyhedron.h>
#include <GeoModelKernel/GeoShapeIntersection.h>
#include <GeoModelKernel/GeoShapeShift.h>
#include <GeoModelKernel/GeoShapeSubtraction.h>
#include <GeoModelKernel/GeoShapeUnion.h>
#include <GeoModelKernel/GeoSimplePolygonBrep.h>
#include <GeoModelKernel/GeoTorus.h>
#include <GeoModelKernel/GeoTrap.h>
#include <GeoModelKernel/GeoTrd.h>
#include <GeoModelKernel/GeoTube.h>
#include <GeoModelKernel/GeoTubs.h>
#include <GeoModelKernel/GeoUnidentifiedShape.h>
#include <GeoModelKernel/GeoVolumeCursor.h>
#include <GeoModelKernel/Units.h>
#include <GeoModelRead/ReadGeoModel.h>
#include <diagnostic_codes.hpp>
#include <ir/geomodel/semantic/importer.hpp>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace nodehammer::ir {
namespace {
using namespace semantic;
glm::dmat4 matrix(const GeoTrf::Transform3D &x) {
    glm::dmat4 out{1.0};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c][r] = x.matrix()(r, c);
    return out;
}
std::string number(double x) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << x;
    return out.str();
}
struct Solid {
    ShapeId id;
    glm::dmat4 frame{1.0};
    bool opaque = false;
};
struct State {
    ImportResult result;
    std::unordered_map<const GeoShape *, Solid> shapes;
    std::unordered_set<const GeoShape *> activeShapes;
    std::unordered_map<const GeoVPhysVol *, LogVolId> volumes;
    std::unordered_map<const GeoMaterial *, MaterialId> materials;
    std::unordered_map<const GeoVPhysVol *, glm::dmat4> frames;
    std::map<OccurrenceTags, uint64_t> tags;
    uint64_t tag(OccurrenceTags values) {
        if (auto it = tags.find(values); it != tags.end())
            return it->second;
        auto id = result.scene.metadata.tagSets.size();
        result.scene.metadata.tagSets.push_back(values);
        tags.emplace(std::move(values), id);
        return id;
    }
    template <class T> Solid boolean(const T *s) {
        auto a = shape(s->getOpA()), b = shape(s->getOpB());
        const auto id = result.scene.nextShapeId();
        const auto relative = glm::inverse(a.frame) * b.frame;
        ShapeVariant value;
        if constexpr (std::is_same_v<T, GeoShapeUnion>)
            value = BooleanUnion{a.id, b.id, relative};
        if constexpr (std::is_same_v<T, GeoShapeIntersection>)
            value = BooleanIntersection{a.id, b.id, relative};
        if constexpr (std::is_same_v<T, GeoShapeSubtraction>)
            value = BooleanSubtraction{a.id, b.id, relative};
        result.scene.shapes.emplace(id, Shape{id, std::move(value)});
        return {id, a.frame, a.opaque || b.opaque};
    }
    Solid shape(const GeoShape *s);
    LogVolId volume(const GeoVPhysVol *v);
};

// Ear clipping of a simple planar polygon; cap triangles retain double coordinates.
TessellatedShape polygon(const GeoSimplePolygonBrep &s) {
    std::vector<glm::dvec2> points;
    for (unsigned i = 0; i < s.getNVertices(); ++i)
        points.emplace_back(s.getXVertex(i), s.getYVertex(i));
    if (points.size() < 3)
        throw std::runtime_error("invalid GeoSimplePolygonBrep");
    auto cross = [](glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; };
    double area = 0;
    for (size_t i = 0; i < points.size(); ++i)
        area += cross(points[i], points[(i + 1) % points.size()]);
    if (area < 0)
        std::reverse(points.begin(), points.end());
    std::vector<size_t> remaining(points.size());
    std::iota(remaining.begin(), remaining.end(), 0);
    TessellatedShape out;
    auto vertex = [&](size_t i, double z) { return glm::dvec3{points[i], z}; };
    while (remaining.size() > 2) {
        bool found = false;
        for (size_t i = 0; i < remaining.size(); ++i) {
            auto a = remaining[(i + remaining.size() - 1) % remaining.size()], b = remaining[i],
                 c = remaining[(i + 1) % remaining.size()];
            if (cross(points[b] - points[a], points[c] - points[b]) <= 0)
                continue;
            bool inside = false;
            for (auto j : remaining)
                if (j != a && j != b && j != c &&
                    cross(points[b] - points[a], points[j] - points[a]) >= 0 &&
                    cross(points[c] - points[b], points[j] - points[b]) >= 0 &&
                    cross(points[a] - points[c], points[j] - points[c]) >= 0) {
                    inside = true;
                    break;
                }
            if (inside)
                continue;
            out.triangles.push_back(
                {{vertex(a, s.getDZ()), vertex(b, s.getDZ()), vertex(c, s.getDZ())}});
            out.triangles.push_back(
                {{vertex(c, -s.getDZ()), vertex(b, -s.getDZ()), vertex(a, -s.getDZ())}});
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
        if (!found)
            throw std::runtime_error(
                "cannot triangulate GeoSimplePolygonBrep without losing geometry");
    }
    for (size_t i = 0; i < points.size(); ++i) {
        auto j = (i + 1) % points.size();
        out.triangles.push_back(
            {{vertex(i, -s.getDZ()), vertex(j, -s.getDZ()), vertex(j, s.getDZ())}});
        out.triangles.push_back(
            {{vertex(i, -s.getDZ()), vertex(j, s.getDZ()), vertex(i, s.getDZ())}});
    }
    return out;
}
Solid State::shape(const GeoShape *s) {
    if (auto it = shapes.find(s); it != shapes.end())
        return it->second;
    if (!activeShapes.insert(s).second)
        throw std::runtime_error("cycle in GeoModel shape graph");
    struct ActiveShapeGuard {
        std::unordered_set<const GeoShape *> &active;
        const GeoShape *shape;
        ~ActiveShapeGuard() { active.erase(shape); }
    } guard{activeShapes, s};
    Solid out;
    if (auto shift = dynamic_cast<const GeoShapeShift *>(s)) {
        out = shape(shift->getOp());
        out.frame = matrix(shift->getX()) * out.frame;
    } else if (auto unionShape = dynamic_cast<const GeoShapeUnion *>(s))
        out = boolean(unionShape);
    else if (auto intersection = dynamic_cast<const GeoShapeIntersection *>(s))
        out = boolean(intersection);
    else if (auto subtraction = dynamic_cast<const GeoShapeSubtraction *>(s))
        out = boolean(subtraction);
    else {
        ShapeVariant value;
        if (auto box = dynamic_cast<const GeoBox *>(s))
            value = BoxShape{box->getXHalfLength(), box->getYHalfLength(), box->getZHalfLength()};
        else if (auto tube = dynamic_cast<const GeoTube *>(s))
            value = TubeShape{tube->getRMin(), tube->getRMax(), tube->getZHalfLength()};
        else if (auto tubeSector = dynamic_cast<const GeoTubs *>(s))
            value = TubeShape{tubeSector->getRMin(), tubeSector->getRMax(),
                              tubeSector->getZHalfLength(), tubeSector->getSPhi(),
                              tubeSector->getDPhi()};
        else if (auto cone = dynamic_cast<const GeoCons *>(s))
            value =
                ConeShape{cone->getRMin1(), cone->getRMax1(), cone->getRMin2(), cone->getRMax2(),
                          cone->getDZ(),    cone->getSPhi(),  cone->getDPhi()};
        else if (auto trd = dynamic_cast<const GeoTrd *>(s))
            value = TrdShape{trd->getXHalfLength1(), trd->getXHalfLength2(), trd->getYHalfLength1(),
                             trd->getYHalfLength2(), trd->getZHalfLength()};
        else if (auto para = dynamic_cast<const GeoPara *>(s))
            value =
                ParaShape{para->getXHalfLength(), para->getYHalfLength(), para->getZHalfLength(),
                          para->getAlpha(),       para->getTheta(),       para->getPhi()};
        else if (auto torus = dynamic_cast<const GeoTorus *>(s))
            value = TorusShape{torus->getRMin(), torus->getRMax(), torus->getRTor(),
                               torus->getSPhi(), torus->getDPhi()};
        else if (auto pcon = dynamic_cast<const GeoPcon *>(s)) {
            PconShape p{pcon->getSPhi(), pcon->getDPhi(), {}};
            for (unsigned i = 0; i < pcon->getNPlanes(); ++i)
                p.sections.push_back(
                    {pcon->getZPlane(i), pcon->getRMinPlane(i), pcon->getRMaxPlane(i)});
            value = std::move(p);
        } else if (auto pgon = dynamic_cast<const GeoPgon *>(s)) {
            PgonShape p{pgon->getSPhi(), pgon->getDPhi(), static_cast<int>(pgon->getNSides()), {}};
            for (unsigned i = 0; i < pgon->getNPlanes(); ++i)
                p.sections.push_back(
                    {pgon->getZPlane(i), pgon->getRMinPlane(i), pgon->getRMaxPlane(i)});
            value = std::move(p);
        } else if (auto trap = dynamic_cast<const GeoGenericTrap *>(s)) {
            GenericTrapShape target;
            target.dz = trap->getZHalfLength();
            const auto &vertices = trap->getVertices();
            if (vertices.size() != 8)
                throw std::runtime_error("GenericTrap requires eight vertices");
            for (size_t i = 0; i < 8; ++i)
                target.vertices[i] = {vertices[i].x(), vertices[i].y()};
            value = target;
        } else if (auto polygonShape = dynamic_cast<const GeoSimplePolygonBrep *>(s))
            value = polygon(*polygonShape);
        else if (dynamic_cast<const GeoTrap *>(s)) {
            GeoPolyhedrizeAction action;
            s->exec(&action);
            auto poly = action.getPolyhedron();
            if (!poly)
                throw std::runtime_error("GeoTrap polyhedrization failed");
            TessellatedShape mesh;
            for (int i = 1; i <= poly->GetNoFacets(); ++i) {
                int n = 0;
                GeoTrf::Vector3D pts[4];
                poly->GetFacet(i, n, pts);
                auto v = [&](int j) { return glm::dvec3{pts[j].x(), pts[j].y(), pts[j].z()}; };
                for (int j = 1; j + 1 < n; ++j)
                    mesh.triangles.push_back({{v(0), v(j), v(j + 1)}});
            }
            value = std::move(mesh);
        } else {
            std::string type = s->type();
            if (auto unidentified = dynamic_cast<const GeoUnidentifiedShape *>(s))
                type += ":" + unidentified->name() + ":" + unidentified->asciiData();
            value = UnknownShape{type};
            out.opaque = true;
            result.diags.error(
                dynamic_cast<const GeoUnidentifiedShape *>(s) ? codes::kErrGeoModelOpaqueShape
                                                              : codes::kErrGeoModelUnsupportedShape,
                "Unrenderable GeoModel shape retained as opaque description: " + type);
        }
        out.id = result.scene.nextShapeId();
        result.scene.shapes.emplace(out.id, Shape{out.id, std::move(value)});
    }
    shapes.emplace(s, out);
    return out;
}
LogVolId State::volume(const GeoVPhysVol *v) {
    if (auto it = volumes.find(v); it != volumes.end())
        return it->second;
    auto &scene = result.scene;
    auto id = scene.nextLogVolId();
    volumes.emplace(v, id);
    const auto *lv = v->getLogVol();
    auto solid = shape(lv->getShape());
    frames[v] = solid.frame;
    const auto *mat = lv->getMaterial();
    auto [mi, added] = materials.try_emplace(mat);
    if (added) {
        mi->second = scene.nextMaterialId();
        scene.materials.emplace(
            mi->second, SourceMaterial{mi->second, mat->getName(), std::nullopt,
                                       mat->getDensity() /
                                           (GeoModelKernelUnits::gram / GeoModelKernelUnits::cm3)});
    }
    LogicalVolume target{id, lv->getName(), solid.id, mi->second};
    OccurrenceTags metadata{{"geomodel.shape", lv->getShape()->type()},
                            {"geomodel.alignment", "nominal"},
                            {"geomodel.length_unit", "mm"}};
    // NHB's material catalog only has density/name. Retain elemental composition in tags.
    for (unsigned i = 0; i < mat->getNumElements(); ++i) {
        const auto *e = mat->getElement(i);
        const auto prefix = "geomodel.material.element." + std::to_string(i) + ".";
        metadata[prefix + "name"] = e->getName();
        metadata[prefix + "symbol"] = e->getSymbol();
        metadata[prefix + "Z"] = number(e->getZ());
        metadata[prefix + "A_g_per_mole"] =
            number(e->getA() / (GeoModelKernelUnits::gram / GeoModelKernelUnits::mole));
        metadata[prefix + "fraction"] = number(mat->getFraction(static_cast<int>(i)));
    }
    if (solid.frame != glm::dmat4{1.0}) {
        std::string frame;
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) {
                if (!frame.empty())
                    frame += ",";
                frame += number(solid.frame[column][row]);
            }
        metadata["geomodel.shape_frame.column_major"] = std::move(frame);
    }
    if (solid.opaque)
        metadata["geomodel.renderable"] = "false";
    scene.metadata.volumeTags[id] = tag(std::move(metadata));
    for (GeoVolumeCursor cursor(v); !cursor.atEnd(); cursor.next()) {
        auto child = cursor.getVolume();
        if (!child)
            throw std::runtime_error("GeoModel virtual surfaces are not supported");
        auto childId = volume(child);
        const auto index = target.daughters.size();
        target.daughters.push_back(
            {cursor.getName(), childId,
             glm::inverse(solid.frame) * matrix(cursor.getDefTransform()) * frames.at(child)});
        OccurrenceTags placement;
        if (auto identifier = cursor.getId())
            placement["geomodel.identifier"] = std::to_string(*identifier);
        if (cursor.hasAlignableTransform())
            placement["geomodel.alignable"] = "true";
        if (!placement.empty())
            scene.metadata.placementTags[{id, index}] = tag(std::move(placement));
    }
    scene.logVols.emplace(id, std::move(target));
    return id;
}
} // namespace
ImportResult importGeoModel(const GeoVPhysVol &root, std::string source) {
    State state;
    state.result.scene.sourceFile = std::move(source);
    auto id = state.volume(&root);
    state.result.scene.root = {root.getLogVol()->getName(), id, state.frames.at(&root)};
    state.result.scene.metadata.defaultSourceSystem = "geomodel";
    (void)state.result.scene.validate();
    return std::move(state.result);
}
std::span<const ImporterOptionSpec> GeoModelImporter::optionSpecs() const {
    static const ImporterOptionSpec specs[] = {
        {"geomodel.allowOpaqueShapes", false, "--geomodel-allow-opaque-shapes",
         "Preserve opaque GeoModel shapes with warnings; these shapes cannot be rendered"}};
    return specs;
}
void GeoModelImporter::configure(const ImporterOptions &options) {
    auto it = options.find("geomodel.allowOpaqueShapes");
    allowOpaqueShapes_ = it != options.end() && std::get<bool>(it->second);
}
ImportResult GeoModelImporter::import(const std::filesystem::path &path) const {
    if (!std::filesystem::is_regular_file(path))
        throw Error{codes::kFatalImportFileNotFound, "GeoModel database does not exist",
                    path.string()};
    try {
        std::ifstream input(path, std::ios::binary);
        char header[16]{};
        input.read(header, 16);
        if (!input || std::string_view(header, 16) != std::string_view("SQLite format 3\0", 16))
            throw std::runtime_error("not a SQLite database");
        auto db = std::make_shared<GMDBManager>(path.string());
        if (!db->checkIsDBOpen())
            throw std::runtime_error("cannot open GeoModel database");
        if (!db->checkTableFromDB("RootVolume") || !db->checkTableFromDB("GeoNodesTypes"))
            throw std::runtime_error("SQLite database is not a GeoModel geometry");
        GeoModelIO::ReadGeoModel reader(db);
        auto root = reader.buildGeoModel();
        if (!root)
            throw std::runtime_error("GeoModel database has no world volume");
        auto result = importGeoModel(*root, path.string());
        if (allowOpaqueShapes_) {
            auto diagnostics = std::move(result.diags).take();
            result.diags = {};
            for (auto &d : diagnostics) {
                if (d.code == codes::kErrGeoModelOpaqueShape)
                    d.severity = Diagnostic::Severity::Warning;
                result.diags.add(std::move(d));
            }
        }
        return result;
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        throw Error{codes::kFatalGeoModelRead, e.what(), path.string()};
    }
}
} // namespace nodehammer::ir
