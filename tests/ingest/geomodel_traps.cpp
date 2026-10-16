#include <GeoModelDBManager/GMDBManager.h>
#include <GeoModelKernel/GeoElement.h>
#include <GeoModelKernel/GeoGenericTrap.h>
#include <GeoModelKernel/GeoMaterial.h>
#include <GeoModelKernel/GeoPhysVol.h>
#include <GeoModelWrite/WriteGeoModel.h>
#include <array>
#include <iostream>
#include <ir/fb/semantic/importer.hpp>
#include <ir/geomodel/semantic/importer.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <limits>
#include <map>
#include <tessellation/generic_trap.hpp>
using namespace nodehammer::ir;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
using Point = std::array<double, 3>;
Point key(glm::dvec3 p) { return {p.x, p.y, p.z}; }
semantic::GenericTrapShape parameters(const GeoGenericTrap &trap) {
    semantic::GenericTrapShape s;
    s.dz = trap.getZHalfLength();
    for (size_t i = 0; i < 8; ++i)
        s.vertices[i] = {trap.getVertices()[i].x(), trap.getVertices()[i].y()};
    return s;
}
semantic::TessellatedShape tessellate(const GeoGenericTrap &trap, double tolerance) {
    return nodehammer::tessellation::tessellateGenericTrap(parameters(trap), tolerance);
}
double verifyClosed(const semantic::TessellatedShape &mesh) {
    std::map<std::pair<Point, Point>, std::pair<int, int>> edges;
    double volume = 0;
    for (const auto &tri : mesh.triangles) {
        const auto &v = tri.vertices;
        check(glm::length(glm::cross(v[1] - v[0], v[2] - v[0])) > 0, "zero-area triangle");
        volume += glm::dot(v[0], glm::cross(v[1], v[2])) / 6;
        for (size_t i = 0; i < 3; ++i) {
            auto a = key(v[i]), b = key(v[(i + 1) % 3]);
            int sign = 1;
            if (b < a) {
                std::swap(a, b);
                sign = -1;
            }
            auto &edge = edges[{a, b}];
            ++edge.first;
            edge.second += sign;
        }
    }
    for (const auto &[edge, counts] : edges)
        check(counts.first == 2 && counts.second == 0,
              "mesh is not closed and consistently oriented");
    check(volume > 0, "mesh winding is inward");
    return volume;
}
template <class F> void rejects(F f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    }
    check(rejected, "invalid input was accepted");
}
int main(int argc, char **argv) {
    try {
        check(argc == 2 || argc == 3, "expected fixture path and optional real database");
        GeoGenericTrapVertices points{{-2, -1}, {2, -1}, {2, 1}, {-2, 1},
                                      {-2, -1}, {2, -1}, {2, 1}, {-2, 1}};
        GeoIntrusivePtr<GeoGenericTrap> box = new GeoGenericTrap(3, points);
        auto flat = tessellate(*box, 0.01);
        check(flat.triangles.size() == 12, "box should use twelve triangles");
        check(std::abs(verifyClosed(flat) - 48) < 1e-10, "box volume");
        const double angle = 0.4;
        for (size_t i = 4; i < 8; ++i) {
            double x = points[i].x(), y = points[i].y();
            points[i] = {std::cos(angle) * x - std::sin(angle) * y,
                         std::sin(angle) * x + std::cos(angle) * y};
        }
        GeoIntrusivePtr<GeoGenericTrap> trap = new GeoGenericTrap(3, points);
        size_t previous = 0;
        for (double tolerance : {0.1, 0.01, 0.001}) {
            auto mesh = tessellate(*trap, tolerance);
            const double volume = verifyClosed(mesh);
            // Section area is quadratic in height; Simpson integration is exact.
            auto areaAt = [&](double t) {
                double area = 0;
                for (size_t i = 0; i < 4; ++i) {
                    auto a = ((1 - t) * points[i] + t * points[i + 4]).eval();
                    auto b = ((1 - t) * points[(i + 1) % 4] + t * points[(i + 1) % 4 + 4]).eval();
                    area += a.x() * b.y() - a.y() * b.x();
                }
                return area / 2;
            };
            const double exactVolume = areaAt(0) + 4 * areaAt(0.5) + areaAt(1);
            check(std::abs(volume - exactVolume) < 60 * tolerance,
                  "twisted trap volume convergence");
            check(mesh.triangles.size() > previous, "tighter tolerance should refine");
            previous = mesh.triangles.size();
            // Independently evaluate the analytic ruled surface at points inside each
            // triangle. Its z and edge coordinate are the same barycentric combination.
            const size_t strips = (mesh.triangles.size() - 4) / 8;
            for (size_t strip = 0; strip < strips; ++strip)
                for (size_t side = 0; side < 4; ++side) {
                    size_t next = (side + 1) % 4;
                    for (size_t half = 0; half < 2; ++half) {
                        const auto &v = mesh.triangles[2 + 8 * strip + 2 * side + half].vertices;
                        for (int a = 0; a <= 8; ++a)
                            for (int b = 0; b <= 8 - a; ++b) {
                                double w0 = a / 8.0, w1 = b / 8.0, w2 = 1 - w0 - w1;
                                glm::dvec3 p = w0 * v[0] + w1 * v[1] + w2 * v[2];
                                double u = half == 0 ? w1 + w2 : w1;
                                double t = (p.z + 3) / 6;
                                GeoTwoVector left = (1 - t) * points[side] + t * points[side + 4];
                                GeoTwoVector right = (1 - t) * points[next] + t * points[next + 4];
                                GeoTwoVector exact = (1 - u) * left + u * right;
                                check(glm::length(p - glm::dvec3{exact.x(), exact.y(), p.z}) <=
                                          tolerance + 1e-12,
                                      "surface error exceeds tolerance");
                            }
                    }
                }
        }
        auto reversed = points;
        for (size_t i = 0; i < 4; ++i) {
            reversed[i] = points[3 - i];
            reversed[i + 4] = points[7 - i];
        }
        GeoIntrusivePtr<GeoGenericTrap> reverse = new GeoGenericTrap(3, reversed);
        verifyClosed(tessellate(*reverse, 0.01));
        auto pyramid = points;
        for (size_t i = 4; i < 8; ++i)
            pyramid[i] = {0, 0};
        GeoIntrusivePtr<GeoGenericTrap> collapsed = new GeoGenericTrap(3, pyramid);
        check(std::abs(verifyClosed(tessellate(*collapsed, 0.1)) - 16) < 1e-9,
              "collapsed top cap volume");
        for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN(), 1e-20})
            rejects([&] { (void)tessellate(*trap, bad); });
        auto invalid = points;
        std::swap(invalid[0], invalid[1]);
        GeoIntrusivePtr<GeoGenericTrap> crossed = new GeoGenericTrap(3, invalid);
        rejects([&] { (void)tessellate(*crossed, 0.01); });
        auto pinchedPoints = points;
        for (size_t i = 0; i < 4; ++i)
            pinchedPoints[i + 4] = -2 * points[i];
        GeoIntrusivePtr<GeoGenericTrap> pinched = new GeoGenericTrap(3, pinchedPoints);
        rejects([&] { (void)tessellate(*pinched, 0.01); });
        auto nonfinitePoints = points;
        nonfinitePoints[0].x() = std::numeric_limits<double>::quiet_NaN();
        GeoIntrusivePtr<GeoGenericTrap> nonfinite = new GeoGenericTrap(3, nonfinitePoints);
        rejects([&] { (void)tessellate(*nonfinite, 0.01); });
        GeoModelImporter importer;

        GeoIntrusivePtr<GeoMaterial> material = new GeoMaterial("test", 1);
        material->add(new GeoElement("hydrogen", "H", 1, 1));
        material->lock();
        GeoIntrusivePtr<GeoPhysVol> root = new GeoPhysVol(new GeoLogVol("trap", trap, material));
        auto memory = importGeoModel(*root, "fixture");
        check(!memory.diags.hasErrors() && memory.diags.size() == 0,
              "parametric import must not warn about tessellation");
        std::filesystem::remove(argv[1]);
        {
            GMDBManager db(argv[1]);
            GeoModelIO::WriteGeoModel writer(db);
            root->exec(&writer);
            writer.saveToDB();
        }

        auto disk = importer.import(argv[1]);
        disk.scene.sourceFile = memory.scene.sourceFile;
        auto bytes = semantic::sceneToBytes(memory.scene);
        // Serialization keeps all 17 doubles, independently of tessellation settings.
        for (auto &[id, shape] : disk.scene.shapes) {
            auto &actual = std::get<semantic::GenericTrapShape>(shape.data);
            const auto &expected =
                std::get<semantic::GenericTrapShape>(memory.scene.shapes.at(id).data);
            check(actual.dz == expected.dz, "SQLite half length");
            for (size_t i = 0; i < 8; ++i)
                check(glm::length(actual.vertices[i] - expected.vertices[i]) < 1e-12,
                      "SQLite coordinate precision");
            actual = expected;
        }
        check(bytes == semantic::sceneToBytes(disk.scene), "SQLite and memory tessellation differ");
        auto restored = FlatBufferImporter::importFromBytes("fixture", bytes);
        check(bytes == semantic::sceneToBytes(restored.scene), "NHB roundtrip differs");
        if (argc == 3) {
            auto real = GeoModelImporter{}.import(argv[2]);
            check(!real.diags.hasErrors(), "real fixture has import errors");
            size_t shapes = 0, triangles = 0;
            for (const auto &[id, volume] : real.scene.logVols) {
                const auto &tags =
                    real.scene.metadata.tagSets.at(real.scene.metadata.volumeTags.at(id));
                if (tags.at("geomodel.shape") != "GenericTrap")
                    continue;
                const auto mesh = nodehammer::tessellation::tessellateGenericTrap(
                    std::get<semantic::GenericTrapShape>(real.scene.shapes.at(volume.shapeId).data),
                    0.01);
                verifyClosed(mesh);
                ++shapes;
                triangles += mesh.triangles.size();
            }
            check(shapes > 0, "real fixture has no generic traps");
            std::cout << "Real fixture: " << shapes << " closed traps, " << triangles
                      << " triangles\n";
        }
        std::cout
            << "GenericTrap error bounds, topology, winding, degeneracies and roundtrips passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
