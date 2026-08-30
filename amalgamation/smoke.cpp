// Proof that the assembled header is a working translation unit.
//
// The one .cpp that defines NH_IMPLEMENTATION, and it includes nothing else --
// which is the claim being tested. Everything below resolves out of the single
// generated header: the public handles, the internal scene it is built from,
// and the FlatBuffer serializer under both.
//
// Built with NH_WITH_TGEO=0 and NH_WITH_DD4HEP=0, set by the target rather
// than here, because this has to run somewhere with no ROOT and no DD4hep --
// an ordinary CI job, or a laptop. Same shipped header as the connector smoke
// test; the two differ only in that answer. What this one exercises is the
// amalgamation, not the importers: that definitions survive the concatenation,
// that `#pragma once` giving way to first-seen-wins did not drop anything, and
// that nothing needed a second translation unit or a link line.

#define NH_IMPLEMENTATION
#include "nodehammer_connect.h"

#include <cstdlib>
#include <iostream>

namespace {

/// A scene built by hand, since no importer is compiled into this variant.
///
/// Reaching for the internal type is the point rather than a shortcut: inside
/// the implementation TU it is visible, exactly as it is inside the library's
/// own sources, and building one here proves the internal half of the header
/// is whole and not merely present.
nodehammer::ir::semantic::Scene makeBox() {
    namespace sem = nodehammer::ir::semantic;
    sem::Scene scene;

    const sem::MaterialId mat = scene.nextMaterialId();
    scene.materials[mat] = {mat, "vacuum", std::nullopt, 0.0};

    const sem::ShapeId shape = scene.nextShapeId();
    scene.shapes[shape] = sem::Shape{shape, sem::BoxShape{10.0, 20.0, 30.0}};

    const sem::LogVolId lv = scene.nextLogVolId();
    scene.logVols[lv] = {lv, "world", shape, mat};

    const sem::NodeId node = scene.nextNodeId();
    sem::Node n;
    n.id = node;
    n.name = "world";
    n.logVolId = lv;
    n.localTransform = glm::dmat4{1.0};
    n.sourceSystem = "smoke";
    scene.nodes[node] = n;
    scene.rootId = node;

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

int check(bool ok, const char *what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    return ok ? 0 : 1;
}

} // namespace

int main() {
    int failures = 0;
    std::cout << "amalgamation smoke test\n";

    // The public handle, built from the internal scene through the same seam
    // the library uses.
    const nodehammer::SemanticScene scene = nodehammer::api::asHandle(makeBox());

    failures += check(scene.valid(), "handle is valid");
    failures += check(scene.nodeCount() == 1, "one node");
    failures += check(scene.logVolCount() == 1, "one logical volume");
    failures += check(scene.shapeCount() == 1, "one shape");
    failures += check(scene.materialCount() == 1, "one material");

    // The serializer, which is the half of the connector that does real work.
    const std::vector<std::byte> nhb = scene.toNhb();
    failures += check(!nhb.empty(), "toNhb produced bytes");

    // `.nhb` is a FlatBuffer with a 4-byte file identifier at offset 4. Checking
    // it is what separates "returned a buffer" from "returned the format".
    bool identified = nhb.size() > 8;
    if (identified) {
        const char *want = "NHS8"; // schemas/semantic.fbs
        for (int i = 0; i < 4; ++i) {
            identified = identified && static_cast<char>(nhb[4 + i]) == want[i];
        }
    }
    failures += check(identified, "bytes carry the .nhb file identifier");

    // The empty handle throws rather than returning something unusable -- the
    // error channel crossed the amalgamation too.
    bool threw = false;
    try {
        (void)nodehammer::SemanticScene{}.toNhb();
    } catch (const nodehammer::Error &) {
        threw = true;
    }
    failures += check(threw, "empty handle throws nodehammer::Error");

    std::cout << (failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
