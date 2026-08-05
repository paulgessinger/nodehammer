// The DD4hep backend, through the shared library.
//
// A different question from test_public_tgeo.cpp's, because DD4hep reaches the
// public surface differently: there is no `read(dd4hep::Detector &)` overload,
// so nothing here is a conditionally-exported symbol and there is no NH_API to
// lose. Every entry point this file touches — `read(path, options)`,
// `formats()` — is exported by every build and already covered.
//
// What is *not* covered anywhere else is whether the backend behind them is
// actually present and loadable in the shared object. DD4hep and ROOT stay
// DT_NEEDED rather than being absorbed the way the static Conan dependencies
// are (CMakeLists.txt's rpath note), so "the library was built with DD4hep" and
// "a consumer can reach DD4hep through the library it links" are two claims,
// and only the first one is cheap. An import that actually runs is what
// separates them: `formats()` would still report "dd4hep" if DDCore failed to
// resolve at load time.
//
// Compiled under NODEHAMMER_WITH_DD4HEP, which in CI means the LCG job.

#include "public_fixture.hpp"

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace nh = nodehammer;

namespace {

// The one place this suite reads geometry it did not build itself, and the
// reason is that DD4hep gives it no choice: the backend's only entry point
// takes a path, so there is no in-memory equivalent of the synthetic importer's
// `read("", {"synthetic"})`.
//
// The checked-in fixture rather than a compact written here, because a usable
// one is not short — DD4hep wants `elements.xml` pulled in through
// ${DD4hepINSTALL} and a material literally named "Vacuum" before it will parse
// anything at all. Restating that here would duplicate domain knowledge
// fixtures/dd4hep/simple_box.xml already carries and tests/import/ already
// maintains, in a suite whose subject is the API surface rather than DD4hep's
// schema.
const std::string kSimpleBox = std::string{NODEHAMMER_FIXTURES_DIR} + "/dd4hep/simple_box.xml";

} // namespace

TEST_CASE("SemanticScene::formats reports dd4hep in a build that has it", "[public][dd4hep]") {
    REQUIRE(nhtest::listed(nh::SemanticScene::formats(), "dd4hep"));
}

TEST_CASE("SemanticScene::read imports a DD4hep compact by name", "[public][dd4hep]") {
    const auto result =
        nh::SemanticScene::read(kSimpleBox, nh::SemanticScene::ReadOptions{"dd4hep"});
    REQUIRE(result.scene.valid());
    REQUIRE_FALSE(result.diags.hasErrors());

    // Exact, from the fixture: a world volume with one box placed inside it.
    // The point of asserting them at all is that reaching this line means DDCore
    // resolved, the importer was registered, and a whole scene crossed the
    // boundary — none of which `formats()` can tell you on its own.
    REQUIRE(result.scene.nodeCount() == 2);
    REQUIRE(result.scene.logVolCount() == 2);
    REQUIRE(result.scene.shapeCount() == 2);
    REQUIRE(result.scene.materialCount() == 2);

    REQUIRE_FALSE(nhtest::anyFatal(result.diags));
}

TEST_CASE("SemanticScene::read recognises a DD4hep compact without being told",
          "[public][dd4hep]") {
    // DD4hep claims no extension — `.xml` belongs to no format on its own — so
    // resolution falls to sniffing the root element. That is a documented
    // behaviour of the path-taking overload and this is the only place it is
    // checked through the public surface.
    const auto sniffed = nh::SemanticScene::read(kSimpleBox);
    REQUIRE(sniffed.scene.valid());
    REQUIRE(sniffed.scene.nodeCount() ==
            nh::SemanticScene::read(kSimpleBox, nh::SemanticScene::ReadOptions{"dd4hep"})
                .scene.nodeCount());
}

TEST_CASE("a DD4hep-imported scene is an ordinary SemanticScene", "[public][dd4hep]") {
    // Same check the TGeo file makes, for the same reason: a scene produced by a
    // conditionally-compiled backend has to be the same kind of handle as any
    // other, since everything downstream of the import is backend-agnostic.
    const auto scene =
        nh::SemanticScene::read(kSimpleBox, nh::SemanticScene::ReadOptions{"dd4hep"}).scene;

    const auto nhb = scene.toNhb();
    REQUIRE_FALSE(nhb.empty());

    const auto reread = nh::SemanticScene::read(std::span<const std::byte>{nhb});
    REQUIRE(reread.scene.nodeCount() == scene.nodeCount());
    REQUIRE(reread.scene.materialCount() == scene.materialCount());
}
