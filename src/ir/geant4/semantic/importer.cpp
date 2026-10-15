#include <diagnostic_codes.hpp>
#include <ir/geant4/semantic/importer.hpp>

#include <G4BooleanSolid.hh>
#include <G4Box.hh>
#include <G4Cons.hh>
#include <G4DisplacedSolid.hh>
#include <G4Element.hh>
#include <G4GDMLParser.hh>
#include <G4Isotope.hh>
#include <G4LogicalVolume.hh>
#include <G4LogicalVolumeStore.hh>
#include <G4Material.hh>
#include <G4PhysicalVolumeStore.hh>
#include <G4Polycone.hh>
#include <G4Polyhedron.hh>
#include <G4SolidStore.hh>
#include <G4StateManager.hh>
#include <G4SystemOfUnits.hh>
#include <G4Trd.hh>
#include <G4Tubs.hh>
#include <G4VExceptionHandler.hh>
#include <G4VPhysicalVolume.hh>
#include <G4ios.hh>

#include <format>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer::ir {
namespace {
// Geant4's geometry stores and GDML reader mutate process state.
std::mutex importMutex;

// Geometry is converted to nodehammer's centimetres; angles stay in radians.
glm::dmat4 transform(const G4RotationMatrix &r, const G4ThreeVector &t) {
    return {glm::dvec4{r.xx(), r.yx(), r.zx(), 0}, glm::dvec4{r.xy(), r.yy(), r.zy(), 0},
            glm::dvec4{r.xz(), r.yz(), r.zz(), 0},
            glm::dvec4{t.x() / cm, t.y() / cm, t.z() / cm, 1}};
}

struct ExceptionHandler final : G4VExceptionHandler {
    DiagnosticList &diags;
    explicit ExceptionHandler(DiagnosticList &d) : diags(d) {}
    G4bool Notify(const char *origin, const char *code, G4ExceptionSeverity severity,
                  const char *description) override {
        const auto message = std::format("Geant4 {}: {}", code, description);
        if (severity == JustWarning) {
            diags.warn(codes::kWarnGdmlBackend, message, origin);
            return false;
        }
        throw Error{codes::kFatalGdmlImport, message, origin};
    }
};

// Restore the embedding application's handler and streams even on parser errors.
struct BackendScope {
    G4VExceptionHandler *previous = G4StateManager::GetStateManager()->GetExceptionHandler();
    ExceptionHandler handler;
    std::ostringstream output;
    std::streambuf *coutBuffer;
    std::streambuf *cerrBuffer;
    explicit BackendScope(DiagnosticList &d)
        : handler(d), coutBuffer(G4cout.rdbuf(output.rdbuf())),
          cerrBuffer(G4cerr.rdbuf(output.rdbuf())) {}
    ~BackendScope() {
        G4cout.rdbuf(coutBuffer);
        G4cerr.rdbuf(cerrBuffer);
        G4StateManager::GetStateManager()->SetExceptionHandler(previous);
    }
};

// Never clean a host application's stores. Delete only objects created by this
// import, including those left behind by a failed GDML read. Material/element
// tables remain Geant4-owned (NIST caches can retain references to them).
template <typename Store> struct NewGeometry {
    Store *store = Store::GetInstance();
    using Pointer = typename Store::value_type;
    std::unordered_set<Pointer> previous{store->begin(), store->end()};
    ~NewGeometry() {
        const std::vector<Pointer> current{store->begin(), store->end()};
        for (auto it = current.rbegin(); it != current.rend(); ++it)
            if (!previous.contains(*it))
                delete *it;
    }
};

// GDML resolves material/element/isotope references through global name lookup,
// independently of SetReverseSearch (which affects geometry). Temporarily hide
// old names so each file resolves its own definitions. Keep objects and table
// indices intact: the NIST caches and an embedding host may own references.
template <typename T> struct HiddenNames {
    std::vector<std::pair<T *, G4String>> saved;
    explicit HiddenNames(const std::vector<T *> &table) {
        saved.reserve(table.size());
        for (auto *entry : table)
            if (entry)
                saved.emplace_back(entry, entry->GetName());
        for (const auto &[entry, name] : saved)
            entry->SetName(
                std::format("__nodehammer_previous_{}", static_cast<const void *>(entry)));
    }
    ~HiddenNames() {
        for (const auto &[entry, name] : saved)
            entry->SetName(name);
    }
};

struct Builder {
    ImportResult result;
    std::unordered_map<const G4LogicalVolume *, semantic::LogVolId> volumes;
    std::unordered_map<const G4VSolid *, semantic::ShapeId> solids;
    std::unordered_map<const G4Material *, semantic::MaterialId> materials;

    semantic::TessellatedShape mesh(const G4VSolid *solid) {
        const auto *poly = solid->GetPolyhedron();
        if (!poly || poly->GetNoFacets() == 0)
            throw Error{codes::kFatalGdmlImport, "Geant4 could not tessellate solid",
                        solid->GetName()};
        semantic::TessellatedShape out;
        for (int face = 1; face <= poly->GetNoFacets(); ++face) {
            G4Point3D vertices[4];
            int count = 0;
            poly->GetFacet(face, count, vertices);
            for (int i = 1; i + 1 < count; ++i) {
                semantic::TessellatedShape::Triangle tri;
                const int indices[]{0, i, i + 1};
                for (std::size_t j = 0; j < 3; ++j) {
                    const auto &v = vertices[indices[j]];
                    tri.vertices[j] = {v.x() / cm, v.y() / cm, v.z() / cm};
                }
                out.triangles.push_back(tri);
            }
        }
        return out;
    }

    semantic::ShapeId shape(const G4VSolid *solid) {
        if (auto it = solids.find(solid); it != solids.end())
            return it->second;
        semantic::ShapeVariant data;
        if (const auto *box = dynamic_cast<const G4Box *>(solid)) {
            data = semantic::BoxShape{box->GetXHalfLength() / cm, box->GetYHalfLength() / cm,
                                      box->GetZHalfLength() / cm};
        } else if (const auto *tube = dynamic_cast<const G4Tubs *>(solid)) {
            data = semantic::TubeShape{tube->GetInnerRadius() / cm, tube->GetOuterRadius() / cm,
                                       tube->GetZHalfLength() / cm, tube->GetStartPhiAngle(),
                                       tube->GetDeltaPhiAngle()};
        } else if (const auto *cone = dynamic_cast<const G4Cons *>(solid)) {
            data = semantic::ConeShape{
                cone->GetInnerRadiusMinusZ() / cm, cone->GetOuterRadiusMinusZ() / cm,
                cone->GetInnerRadiusPlusZ() / cm,  cone->GetOuterRadiusPlusZ() / cm,
                cone->GetZHalfLength() / cm,       cone->GetStartPhiAngle(),
                cone->GetDeltaPhiAngle()};
        } else if (const auto *trd = dynamic_cast<const G4Trd *>(solid)) {
            data = semantic::TrdShape{trd->GetXHalfLength1() / cm, trd->GetXHalfLength2() / cm,
                                      trd->GetYHalfLength1() / cm, trd->GetYHalfLength2() / cm,
                                      trd->GetZHalfLength() / cm};
        } else if (const auto *polycone = dynamic_cast<const G4Polycone *>(solid)) {
            const auto *p = polycone->GetOriginalParameters();
            semantic::PconShape out{p->Start_angle, p->Opening_angle, {}};
            for (int i = 0; i < p->Num_z_planes; ++i)
                out.sections.push_back({p->Z_values[i] / cm, p->Rmin[i] / cm, p->Rmax[i] / cm});
            data = std::move(out);
        } else if (const auto *boolean = dynamic_cast<const G4BooleanSolid *>(solid)) {
            const auto *left = boolean->GetConstituentSolid(0);
            const auto *right = boolean->GetConstituentSolid(1);
            glm::dmat4 rightTransform{1};
            while (const auto *displaced = dynamic_cast<const G4DisplacedSolid *>(right)) {
                rightTransform *=
                    transform(displaced->GetObjectRotation(), displaced->GetObjectTranslation());
                right = displaced->GetConstituentMovedSolid();
            }
            const auto a = shape(left), b = shape(right);
            const auto type = solid->GetEntityType();
            if (type == "G4UnionSolid")
                data = semantic::BooleanUnion{a, b, rightTransform};
            else if (type == "G4IntersectionSolid")
                data = semantic::BooleanIntersection{a, b, rightTransform};
            else if (type == "G4SubtractionSolid")
                data = semantic::BooleanSubtraction{a, b, rightTransform};
            else
                throw Error{codes::kFatalGdmlImport, "unsupported Geant4 boolean", type};
        } else {
            data = mesh(solid);
            result.diags.info(
                codes::kInfoGdmlTessellated,
                std::format("using Geant4 polyhedron for {}", std::string{solid->GetEntityType()}),
                solid->GetName());
        }
        auto id = result.scene.nextShapeId();
        result.scene.shapes.emplace(id, semantic::Shape{id, std::move(data)});
        solids.emplace(solid, id);
        return id;
    }

    semantic::MaterialId material(const G4Material *m) {
        if (auto it = materials.find(m); it != materials.end())
            return it->second;
        auto id = result.scene.nextMaterialId();
        result.scene.materials.emplace(
            id, semantic::SourceMaterial{id, m->GetName(), {}, m->GetDensity() / (g / cm3)});
        materials.emplace(m, id);
        return id;
    }

    uint64_t tags(const G4VPhysicalVolume *pv) {
        auto &sets = result.scene.metadata.tagSets;
        const auto index = static_cast<uint64_t>(sets.size());
        sets.push_back({{"copyNumber", std::to_string(pv->GetCopyNo())}});
        return index;
    }

    semantic::LogVolId volume(const G4LogicalVolume *lv) {
        if (auto it = volumes.find(lv); it != volumes.end())
            return it->second;
        const auto id = result.scene.nextLogVolId();
        volumes.emplace(lv, id);
        semantic::LogicalVolume out{id, lv->GetName(), shape(lv->GetSolid()),
                                    material(lv->GetMaterial())};
        for (std::size_t i = 0; i < lv->GetNoDaughters(); ++i) {
            const auto *pv = lv->GetDaughter(i);
            if (pv->IsReplicated() || pv->IsParameterised())
                throw Error{
                    codes::kFatalGdmlImport,
                    "replica and parameterised placements are not supported by the GDML importer",
                    pv->GetName()};
            const auto child = volume(pv->GetLogicalVolume());
            result.scene.metadata.placementTags[{id, out.daughters.size()}] = tags(pv);
            out.daughters.push_back(
                {pv->GetName(), child,
                 transform(pv->GetObjectRotationValue(), pv->GetObjectTranslation())});
        }
        result.scene.logVols.emplace(id, std::move(out));
        return id;
    }
};
} // namespace

ImportResult GdmlImporter::import(const std::filesystem::path &path) const {
    std::unique_lock lock(importMutex, std::try_to_lock);
    if (!lock.owns_lock())
        throw Error{codes::kFatalGdmlImport, "GDML imports cannot run concurrently"};
    if (!std::filesystem::is_regular_file(path))
        throw Error{codes::kFatalImportFileNotFound,
                    std::format("GDML file not found: {}", path.string()), path.string()};
    Builder builder;
    BackendScope backend(builder.result.diags);
    // Destruction order: parser, placements, volumes, solids; never global Clean().
    NewGeometry<G4SolidStore> solids;
    NewGeometry<G4LogicalVolumeStore> volumes;
    NewGeometry<G4PhysicalVolumeStore> placements;
    try {
        G4GDMLParser parser;
        parser.SetStripFlag(false);
        parser.SetReverseSearch(true);
        {
            HiddenNames<G4Material> materials(*G4Material::GetMaterialTable());
            HiddenNames<G4Element> elements(*G4Element::GetElementTable());
            HiddenNames<G4Isotope> isotopes(*G4Isotope::GetIsotopeTable());
            parser.Read(path.string(), false); // no dependency on a remote schema URL
        }
        const auto *world = parser.GetWorldVolume();
        if (!world)
            throw Error{codes::kFatalGdmlImport, "GDML has no world volume", path.string()};
        auto &scene = builder.result.scene;
        scene.sourceFile = path.string();
        scene.metadata.defaultSourceSystem = "geant4";
        scene.root = {world->GetName(), builder.volume(world->GetLogicalVolume()),
                      transform(world->GetObjectRotationValue(), world->GetObjectTranslation())};
        scene.metadata.occurrenceTags[{}] = builder.tags(world);
        const auto count = scene.validate();
        builder.result.diags.debug(
            codes::kDebugImportStats,
            std::format("{} occurrences, {} logical volumes", count, scene.logVols.size()));
        return std::move(builder.result);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &ex) {
        throw Error{codes::kFatalGdmlImport, ex.what(), path.string()};
    }
}
} // namespace nodehammer::ir
