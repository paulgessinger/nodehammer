// The connector header doing the thing it exists for.
//
// An experiment builds geometry in its own process and hands it over; bytes
// come back. That is the whole tier, and this is it end to end -- no file is
// read, no file is written, and the only nodehammer artifact involved is one
// generated header.
//
// Compiles only where ROOT and DD4hep do, which is CI's LCG job. That is not a
// limitation of the test but its subject: nodehammer_connect.h leaves those two
// as real `#include`s precisely because the experiment already has them, so a
// build that has them is the only one that can prove the header is complete.
// The ROOT-free half of the same machinery is covered by smoke.cpp, which runs
// everywhere.
//
// What it adds over that one:
//
//   * The importers survive the amalgamation. They are the largest absorbed
//     sources and the only ones that include third-party headers left
//     unresolved, so they are where an include-order mistake would surface.
//   * `tgeoMatrixToGlm` resolves to one definition. Two anonymous-namespace
//     copies compiled fine as separate translation units and are a redefinition
//     once concatenated -- docs/event-display-design.md §9.2 predicted exactly
//     this, and a build that never concatenates them cannot notice.
//   * The `#if NH_WITH_TGEO` gates the absorbed sources carry are satisfied by
//     the header itself, not by this file's build system.

#define NH_IMPLEMENTATION
#include "nodehammer_connect.h"

#include <cstdlib>
#include <iostream>

#include <TGeoManager.h>
#include <TGeoMaterial.h>
#include <TGeoMedium.h>
#include <TGeoVolume.h>

namespace {

/// A world box with one volume placed inside it twice.
///
/// The same shape tests/public/test_public_tgeo.cpp builds, and for the same
/// reason: one logical volume across two placements is what makes the counts
/// below mean something, since they differ exactly when the importer stops
/// sharing the volume.
TGeoManager *makeGeometry() {
    auto *mgr = new TGeoManager("amalgam_smoke", "amalgam_smoke");
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);

    auto *top = mgr->MakeBox("world", med, 100.0, 100.0, 100.0);
    mgr->SetTopVolume(top);

    auto *child = mgr->MakeBox("child", med, 10.0, 10.0, 10.0);
    top->AddNode(child, 1);
    top->AddNode(child, 2);

    mgr->CloseGeometry();
    return mgr;
}

int check(bool ok, const char *what) {
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    return ok ? 0 : 1;
}

} // namespace

int main() {
    int failures = 0;
    std::cout << "amalgamated connector smoke test\n";

    TGeoManager *mgr = makeGeometry();

    // The entry point the tier exists for: a manager the caller owns, traversed
    // in place. Never touches gGeoManager, which the library's own suite checks
    // in detail; here the claim being tested is only that it is reachable and
    // whole through the amalgamation.
    const auto result = nodehammer::SemanticScene::read(*mgr);

    failures += check(result.scene.valid(), "read(TGeoManager &) produced a scene");
    failures += check(!result.diags.hasErrors(), "import reported no errors");

    // Exact, from the geometry above: world plus two placements is three nodes
    // over two logical volumes, two box shapes, one shared medium. The gap
    // between 3 and 2 is the assertion that matters.
    failures += check(result.scene.nodeCount() == 3, "three nodes");
    failures += check(result.scene.logVolCount() == 2, "two logical volumes");
    failures += check(result.scene.shapeCount() == 2, "two shapes");
    failures += check(result.scene.materialCount() == 1, "one material");

    // The other half of the tier: bytes, which is what goes on a socket.
    const std::vector<std::byte> nhb = result.scene.toNhb();
    failures += check(!nhb.empty(), "toNhb produced bytes");

    bool identified = nhb.size() > 8;
    if (identified) {
        const char *want = "NHS8"; // schemas/semantic.fbs
        for (int i = 0; i < 4; ++i) {
            identified = identified && static_cast<char>(nhb[4 + i]) == want[i];
        }
    }
    failures += check(identified, "bytes carry the .nhb file identifier");

    // The DD4hep entry point is compiled in -- connect.json names that importer
    // -- but building a Detector needs a compact XML this test has no business
    // carrying. Taking its address is enough to prove the symbol was defined
    // rather than merely declared, which is the failure the amalgamation could
    // plausibly produce.
    using Dd4hepRead = nodehammer::SemanticResult (*)(dd4hep::Detector &);
    const Dd4hepRead dd4hepRead = &nodehammer::SemanticScene::read;
    failures += check(dd4hepRead != nullptr, "read(dd4hep::Detector &) is defined");

    std::cout << (failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
