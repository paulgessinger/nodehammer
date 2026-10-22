#include <GeoGenericFunctions/Variable.h>
#include <GeoModelDBManager/GMDBManager.h>
#include <GeoModelKernel/GeoAlignableTransform.h>
#include <GeoModelKernel/GeoBox.h>
#include <GeoModelKernel/GeoElement.h>
#include <GeoModelKernel/GeoIdentifierTag.h>
#include <GeoModelKernel/GeoMaterial.h>
#include <GeoModelKernel/GeoNameTag.h>
#include <GeoModelKernel/GeoPgon.h>
#include <GeoModelKernel/GeoPhysVol.h>
#include <GeoModelKernel/GeoSerialDenominator.h>
#include <GeoModelKernel/GeoSerialIdentifier.h>
#include <GeoModelKernel/GeoSerialTransformer.h>
#include <GeoModelKernel/GeoShapeShift.h>
#include <GeoModelKernel/GeoShapeSubtraction.h>
#include <GeoModelKernel/GeoSimplePolygonBrep.h>
#include <GeoModelKernel/GeoUnidentifiedShape.h>
#include <GeoModelKernel/Units.h>
#include <GeoModelWrite/WriteGeoModel.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ir/fb/semantic/importer.hpp>
#include <ir/geomodel/semantic/importer.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <stdexcept>
using namespace nodehammer::ir;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    try {
        if (argc != 2)
            return 2;
        GeoIntrusivePtr<GeoMaterial> material =
            new GeoMaterial("silicon", 2.33 * GeoModelKernelUnits::gram / GeoModelKernelUnits::cm3);
        material->add(new GeoElement(
            "silicon", "Si", 14, 28.0855 * GeoModelKernelUnits::gram / GeoModelKernelUnits::mole));
        material->lock();
        GeoIntrusivePtr<GeoBox> box = new GeoBox(1, 2, 3);
        GeoIntrusivePtr<GeoLogVol> logical = new GeoLogVol("shared-logical", box, material);
        GeoIntrusivePtr<GeoPhysVol> root =
            new GeoPhysVol(new GeoLogVol("world", new GeoBox(100, 100, 100), material));
        GeoIntrusivePtr<GeoPhysVol> a = new GeoPhysVol(logical), b = new GeoPhysVol(logical),
                                    leaf = new GeoPhysVol(logical);
        a->add(new GeoTransform(GeoTrf::Translate3D(0, 4, 0)));
        a->add(leaf);
        root->add(new GeoNameTag("a"));
        root->add(new GeoIdentifierTag(42));
        root->add(new GeoTransform(GeoTrf::Translate3D(10, 0, 0) *
                                   GeoTrf::RotateZ3D(std::numbers::pi / 2)));
        root->add(a);
        root->add(new GeoNameTag("b"));
        root->add(b);
        root->add(new GeoSerialDenominator("copy"));
        root->add(new GeoSerialIdentifier(100));
        GeoGenfun::Variable index;
        GeoXF::Pow transform(GeoTrf::Translate3D(0, 0, 5), index);
        root->add(new GeoSerialTransformer(leaf, &transform, 3));
        // Both boolean operands are shifted; the left frame must be carried into
        // the placement, and removed from its daughters' placements.
        GeoIntrusivePtr<GeoShape> left = new GeoShapeShift(box, GeoTrf::Translate3D(7, 0, 0));
        GeoIntrusivePtr<GeoShape> right = new GeoShapeShift(box, GeoTrf::Translate3D(8, 0, 0));
        GeoIntrusivePtr<GeoPhysVol> shifted = new GeoPhysVol(
            new GeoLogVol("shifted", new GeoShapeSubtraction(left, right), material));
        shifted->add(new GeoTransform(GeoTrf::Translate3D(0, 9, 0)));
        shifted->add(leaf);
        root->add(shifted);
        auto result = importGeoModel(*root);
        const auto &scene = result.scene;
        check(scene.nodeCount() == 9, "occurrence count including serial transformer");
        const auto &d = scene.logVols.at(scene.root.logVolId).daughters;
        check(d.size() == 6, "daughter count");
        check(d[0].logVolId != d[1].logVolId,
              "physical volumes sharing a logvol must stay distinct");
        auto nested =
            d[0].localTransform * scene.logVols.at(d[0].logVolId).daughters[0].localTransform;
        check(std::abs(nested[3].x - 6) < 1e-12 && std::abs(nested[3].y) < 1e-12,
              "nested rotation/translation composition");
        check(d[2].logVolId == d[3].logVolId && d[3].logVolId == d[4].logVolId,
              "serial placement sharing");
        check(d[0].localTransform[3].x == 10 && d[4].localTransform[3].z == 10,
              "translation and serial transform");
        const auto &tags =
            scene.metadata.tagSets.at(scene.metadata.placementTags.at({scene.root.logVolId, 0}));
        check(tags.at("geomodel.identifier") == "42", "identifier");
        check(std::abs(scene.materials.begin()->second.density - 2.33) < 1e-12, "density units");
        auto shift = d[5].localTransform;
        check(shift[3].x == 7, "left operand frame");
        const auto &shiftedVolume = scene.logVols.at(d[5].logVolId);
        auto world = shift * shiftedVolume.daughters[0].localTransform;
        check(world[3].x == 0 && world[3].y == 9, "shape frame must not displace children");
        const auto &boolean =
            std::get<semantic::BooleanSubtraction>(scene.shapes.at(shiftedVolume.shapeId).data);
        check(boolean.rightTransform[3].x == 1, "relative boolean operand transform");
        auto bytes = semantic::sceneToBytes(scene);
        auto restored = FlatBufferImporter::importFromBytes("roundtrip.nhb", bytes);
        check(semantic::sceneToBytes(restored.scene) == bytes, "NHB round-trip");
        const std::filesystem::path dbPath = argv[1];
        if (std::filesystem::exists(dbPath))
            std::filesystem::remove(dbPath);
        {
            GMDBManager db(dbPath.string());
            GeoModelIO::WriteGeoModel writer(db);
            root->exec(&writer);
            writer.saveToDB();
        }
        auto loaded = GeoModelImporter{}.import(dbPath);
        loaded.scene.sourceFile = scene.sourceFile;
        // GeoModel's SQLite writer rounds transform coefficients (e.g. cos(pi/2)
        // from 6.123e-17 to 1e-16). Check the actual difference before normalizing
        // only those coefficients for an otherwise byte-exact comparison.
        auto compareTransform = [](glm::dmat4 &actual, const glm::dmat4 &expected) {
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    check(std::abs(actual[c][r] - expected[c][r]) < 1e-12,
                          "SQLite transform precision");
            actual = expected;
        };
        compareTransform(loaded.scene.root.localTransform, scene.root.localTransform);
        for (auto &[id, volume] : loaded.scene.logVols) {
            const auto &expected = scene.logVols.at(id);
            check(volume.daughters.size() == expected.daughters.size(), "SQLite daughter count");
            for (size_t i = 0; i < volume.daughters.size(); ++i)
                compareTransform(volume.daughters[i].localTransform,
                                 expected.daughters[i].localTransform);
        }
        check(semantic::sceneToBytes(loaded.scene) == bytes,
              "SQLite and memory conversion must agree apart from writer rounding");
        // Planar concave caps and both input windings must form the same closed solid.
        for (bool reverse : {false, true}) {
            GeoIntrusivePtr<GeoSimplePolygonBrep> poly = new GeoSimplePolygonBrep(2);
            std::vector<std::pair<double, double>> vertices{{0, 0}, {3, 0}, {3, 1},
                                                            {1, 1}, {1, 3}, {0, 3}};
            if (reverse)
                std::reverse(vertices.begin(), vertices.end());
            for (auto [x, y] : vertices)
                poly->addVertex(x, y);
            GeoIntrusivePtr<GeoPhysVol> pv =
                new GeoPhysVol(new GeoLogVol("concave", poly, material));
            auto converted = importGeoModel(*pv);
            auto shapeId = converted.scene.logVols.at(converted.scene.root.logVolId).shapeId;
            const auto &mesh =
                std::get<semantic::TessellatedShape>(converted.scene.shapes.at(shapeId).data);
            double volume = 0;
            for (const auto &tri : mesh.triangles)
                volume +=
                    glm::dot(tri.vertices[0], glm::cross(tri.vertices[1], tri.vertices[2])) / 6;
            check(std::abs(volume - 20) < 1e-10, "concave polygon winding/volume");
        }
        // Nominal alignment deliberately ignores an applied delta.
        GeoIntrusivePtr<GeoPhysVol> aligned = new GeoPhysVol(logical);
        GeoIntrusivePtr<GeoAlignableTransform> alignment =
            new GeoAlignableTransform(GeoTrf::Translate3D(2, 0, 0));
        alignment->setDelta(GeoTrf::Translate3D(10, 0, 0));
        aligned->add(alignment);
        aligned->add(leaf);
        auto nominal = importGeoModel(*aligned);
        check(nominal.scene.logVols.at(nominal.scene.root.logVolId)
                      .daughters[0]
                      .localTransform[3]
                      .x == 2,
              "nominal alignment");
        GeoIntrusivePtr<GeoPhysVol> opaque = new GeoPhysVol(new GeoLogVol(
            "opaque", new GeoUnidentifiedShape("extension", "payload", 123), material));
        auto opaqueResult = importGeoModel(*opaque);
        check(opaqueResult.diags.hasErrors(), "opaque shape must report an error by default");
        const auto &unknown =
            std::get<semantic::UnknownShape>(opaqueResult.scene.shapes.begin()->second.data);
        check(unknown.originalType.find("payload") != std::string::npos, "opaque payload retained");
        auto opaquePath = dbPath.string() + ".opaque.db";
        std::filesystem::remove(opaquePath);
        {
            GMDBManager db(opaquePath);
            GeoModelIO::WriteGeoModel writer(db);
            opaque->exec(&writer);
            writer.saveToDB();
        }
        GeoModelImporter permissive;
        permissive.configure({{"geomodel.allowOpaqueShapes", true}});
        auto allowed = permissive.import(opaquePath);
        check(!allowed.diags.hasErrors() && allowed.diags.size() == 1,
              "opaque shape opt-in must retain a warning");
        const auto invalidPath = dbPath.string() + ".invalid";
        {
            std::ofstream bad(invalidPath);
            bad << "not sqlite";
        }
        bool rejected = false;
        try {
            (void)GeoModelImporter{}.import(invalidPath);
        } catch (const nodehammer::Error &) {
            rejected = true;
        }
        check(rejected, "reject non-SQLite input");
        std::filesystem::remove(invalidPath);
        std::cout << "GeoModel traversal, sharing, transforms, identifiers, units, SQLite and NHB "
                     "round-trips passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
