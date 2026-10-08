#include <nodehammer/io.hpp>
#include <nodehammer/nhb.hpp>
#include <nodehammer/tgeo.hpp>
// Exercise fromTGeo through the shared library with caller-owned ROOT geometry.
// This header and these tests are available only in TGeo-enabled builds.

#include "public_fixture.hpp"

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <TGeoManager.h>
#include <TGeoMaterial.h>
#include <TGeoMedium.h>
#include <TGeoVolume.h>

#include <cstddef>
#include <span>

namespace nh = nodehammer;

namespace {

/// Delete any lingering `gGeoManager` and start fresh.
///
/// ROOT keeps one global manager and a fresh `TGeoManager` installs itself
/// there, so cases in one process would otherwise inherit each other's
/// geometry. Same helper, and same reason, as tests/import/test_tgeo_importer.
void resetManager() {
    delete gGeoManager;
    gGeoManager = nullptr;
}

/// A world box with one child volume placed inside it `placements` times.
///
/// Closed and ready to traverse. One volume placed repeatedly rather than
/// `placements` separate `MakeBox` calls, because that is what makes the counts
/// below say something: the two differ only in whether the importer reuses a
/// logical volume across placements, and this shape of geometry is the one that
/// notices if it stops. Every volume shares the one medium, which is what fixes
/// the material count at 1 regardless of `placements`.
TGeoManager *makeGeometry(const char *name, int placements) {
    auto *mgr = new TGeoManager(name, name);
    auto *mat = new TGeoMaterial("vacuum", 0, 0, 0);
    auto *med = new TGeoMedium("vacuum", 1, mat);

    auto *top = mgr->MakeBox("world", med, 100.0, 100.0, 100.0);
    mgr->SetTopVolume(top);

    auto *child = mgr->MakeBox("child", med, 10.0, 10.0, 10.0);
    for (int i = 0; i < placements; ++i) {
        top->AddNode(child, i + 1);
    }

    mgr->CloseGeometry();
    return mgr;
}

} // namespace

TEST_CASE("semanticReadFormats reports tgeo in a build that has ROOT", "[public][tgeo]") {
    // The runtime half of the capability question. The compile-time half is
    // this file existing at all — and the two have to agree, because a consumer
    // that checks `formats()` before calling `read` is relying on exactly that.
    REQUIRE(nhtest::listed(nh::semanticReadFormats(), "tgeo"));
}

TEST_CASE("fromTGeo traverses a caller-owned TGeoManager", "[public][tgeo]") {
    resetManager();
    auto *mgr = makeGeometry("public_tgeo_read", 2);

    const auto result = nh::fromTGeo(*mgr);
    REQUIRE(result.scene.valid());
    REQUIRE_FALSE(result.diags.hasErrors());

    // Exact, because the geometry above is: the world plus its two placements is
    // three nodes over two logical volumes — "world" and the one "child" both
    // placements share — two box shapes, and the single medium every volume was
    // built from. The gap between 3 nodes and 2 logical volumes is the whole
    // assertion: it is what says the placements were shared rather than copied.
    REQUIRE(result.scene.nodeCount() == 3);
    REQUIRE(result.scene.logVolCount() == 2);
    REQUIRE(result.scene.shapeCount() == 2);
    REQUIRE(result.scene.materialCount() == 1);

    // Invariant 1 of docs/error-model.md, over the one returned list the rest of
    // the suite cannot produce.
    REQUIRE_FALSE(nhtest::anyFatal(result.diags));
}

TEST_CASE("fromTGeo never touches gGeoManager", "[public][tgeo]") {
    // The header promises this, and it is the reason the overload takes a
    // reference rather than reading the global itself: a consumer with its own
    // manager — one of several, or one it never installed — needs to know which
    // geometry it just imported.
    //
    // Not checkable from tests/import/, which hands the importer `gGeoManager`
    // itself and so cannot tell a traversal of the argument from a traversal of
    // the global.
    //
    // Checked by emptying the global rather than by pointing it at a second
    // geometry: two live TGeoManagers share more ROOT state than they appear to
    // — materials and media land in whichever one is current — so a two-manager
    // version tests ROOT's tolerance for that as much as it tests nodehammer.
    // A null global has no such ambiguity. An implementation that reached for
    // `gGeoManager` would dereference nothing at all, which is the loudest
    // possible failure and exactly what should happen.
    resetManager();
    auto *mine = makeGeometry("public_tgeo_owned", 2);
    REQUIRE(gGeoManager == mine); // ROOT installed it on construction

    gGeoManager = nullptr;
    const auto result = nh::fromTGeo(*mine);

    REQUIRE(gGeoManager == nullptr);        // nothing installed on the way out
    REQUIRE(result.scene.nodeCount() == 3); // and the argument was what it read

    // Put it back so the next case's resetManager() has something to delete;
    // `mine` is otherwise unreachable and would leak.
    gGeoManager = mine;
}

TEST_CASE("a TGeo-imported scene round-trips through .nhb", "[public][tgeo]") {
    // Everything downstream of the import is backend-agnostic, so this is not
    // re-testing the writer. What it checks is that a scene built by the
    // conditionally-compiled path is the same kind of handle as any other —
    // that the ROOT build does not hand back something subtly its own.
    resetManager();
    auto *mgr = makeGeometry("public_tgeo_roundtrip", 3);

    const auto scene = nh::fromTGeo(*mgr).scene;
    const auto nhb = nh::toNhb(scene);
    REQUIRE_FALSE(nhb.empty());

    const auto reread = nh::fromNhb(std::span<const std::byte>{nhb});
    REQUIRE(reread.scene.nodeCount() == scene.nodeCount());
    // NHS8 stores physical topology. Its lossless canonical adapter uses one
    // definition per stored node; the next stack stage preserves sharing on disk.
    REQUIRE(reread.scene.logVolCount() == scene.nodeCount());
    REQUIRE(reread.scene.shapeCount() == scene.shapeCount());
    REQUIRE(reread.scene.materialCount() == scene.materialCount());
}
