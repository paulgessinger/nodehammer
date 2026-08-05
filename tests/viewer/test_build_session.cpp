#include <catch2/catch_test_macros.hpp>

#include <detail/file_io.hpp>
#include <detail/zstd_io.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/semantic.hpp>
#include <viewer/bag_project_fs.hpp>
#include <viewer/build_session.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace nodehammer;
using namespace nodehammer::ir;
using nodehammer::viewer::BagProjectFs;
using nodehammer::viewer::BuildPhase;
using nodehammer::viewer::BuildSession;
using nodehammer::viewer::CapturingLogSink;

namespace {

constexpr int kPollBudget = 100;

ir::semantic::Scene makeMinimalScene() {
    ir::semantic::Scene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, ir::semantic::BoxShape{5.0, 10.0, 15.0}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "iron", glm::vec3{0.5f, 0.5f, 0.5f}, 7.87};

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "ironBox", shapeId, matId};

    auto nodeId = scene.nextNodeId();
    ir::semantic::Node node;
    node.id = nodeId;
    node.name = "root";
    node.logVolId = lvId;
    node.sourceSystem = "test";
    scene.nodes[nodeId] = node;
    scene.rootId = nodeId;
    scene.sourceFile = "/test/input";

    return scene;
}

std::vector<std::byte> minimalNhbZstBytes() {
    auto raw = semanticSceneToBytes(makeMinimalScene());
    return detail::zstd_io::compress(std::span<const std::byte>{raw});
}

std::vector<std::byte> stringBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

void refreshUntilSettled(BuildSession &session, viewer::ProjectFs &project) {
    for (int i = 0; i < kPollBudget; ++i) {
        const auto p = session.phase();
        if (p == BuildPhase::ResolvedReady || p == BuildPhase::WaitingForUser ||
            p == BuildPhase::Error || p == BuildPhase::Idle || p == BuildPhase::Consumed) {
            // For Walking we keep polling; bag is synchronous so resolved
            // states settle within a couple of polls.
            if (p != BuildPhase::Idle) {
                return;
            }
        }
        session.refresh(project);
    }
}

} // namespace

TEST_CASE("BuildSession resolves a flat config + geometry from a bag", "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("# minimal nodehammer config\n"));
    auto geom = minimalNhbZstBytes();
    bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    for (int i = 0; i < kPollBudget; ++i) {
        session.refresh(*bag);
        if (session.phase() == BuildPhase::ResolvedReady || session.phase() == BuildPhase::Error ||
            session.phase() == BuildPhase::WaitingForUser) {
            break;
        }
    }

    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
    auto inputs = session.takeInputs();
    REQUIRE(inputs);
    REQUIRE_FALSE(inputs->config.diags.hasErrors());
    REQUIRE_FALSE(inputs->import.diags.hasErrors());
    REQUIRE(inputs->import.scene.nodes.contains(inputs->import.scene.rootId));
}

TEST_CASE("BuildSession input_hash is content-addressed and backend-independent",
          "[viewer][build_session]") {
    auto geom = minimalNhbZstBytes();

    auto build = [&](viewer::ProjectFs &project) {
        BuildSession session;
        session.setRootKeys("scene.toml", "scene.nhb.zst");
        for (int i = 0; i < kPollBudget; ++i) {
            session.refresh(project);
            if (session.phase() == BuildPhase::ResolvedReady) {
                break;
            }
        }
        REQUIRE(session.phase() == BuildPhase::ResolvedReady);
        auto inputs = session.takeInputs();
        REQUIRE(inputs);
        return inputs->input_hash;
    };

    // Same bytes in two independent bags → identical hash (so a backend swap that
    // resolves the same content memoizes instead of re-tessellating).
    auto bagA = std::make_unique<BagProjectFs>();
    bagA->addBytes("scene.toml", stringBytes("# minimal nodehammer config\n"));
    bagA->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});
    const auto hashA = build(*bagA);

    auto bagB = std::make_unique<BagProjectFs>();
    bagB->addBytes("scene.toml", stringBytes("# minimal nodehammer config\n"));
    bagB->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});
    REQUIRE(build(*bagB) == hashA);
    REQUIRE(hashA != 0);

    // A one-byte config change flips the hash.
    auto bagC = std::make_unique<BagProjectFs>();
    bagC->addBytes("scene.toml", stringBytes("# minimal nodehammer config!\n"));
    bagC->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});
    REQUIRE(build(*bagC) != hashA);
}

TEST_CASE("BuildSession surfaces missing geometry as WaitingForUser", "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("# minimal\n"));

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    refreshUntilSettled(session, *bag);

    REQUIRE(session.phase() == BuildPhase::WaitingForUser);
    bool found = false;
    for (const auto &k : session.missing()) {
        if (k == "scene.nhb.zst") {
            found = true;
            break;
        }
    }
    REQUIRE(found);
}

TEST_CASE("BuildSession resolves subdir include via bag basename fallback",
          "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("include = [\"subdir/common.toml\"]\n"));
    bag->addBytes("common.toml",
                  stringBytes("[materials.steel]\nmetallic = 0.8\nroughness = 0.2\n"));
    auto geom = minimalNhbZstBytes();
    bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    for (int i = 0; i < kPollBudget; ++i) {
        session.refresh(*bag);
        if (session.phase() == BuildPhase::ResolvedReady || session.phase() == BuildPhase::Error ||
            session.phase() == BuildPhase::WaitingForUser) {
            break;
        }
    }

    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
}

TEST_CASE("BuildSession enters Error on malformed FlatBuffer geometry", "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("# minimal\n"));
    bag->addBytes("scene.nhb.zst", stringBytes("not a real flatbuffer payload"));

    BuildSession session;
    CapturingLogSink sink;
    session.setLogSink(&sink);
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    for (int i = 0; i < kPollBudget; ++i) {
        session.refresh(*bag);
        if (session.phase() == BuildPhase::Error || session.phase() == BuildPhase::ResolvedReady) {
            break;
        }
    }

    REQUIRE(session.phase() == BuildPhase::Error);
    REQUIRE(sink.hasErrors());
}

// ── Lua configs ──────────────────────────────────────────────────────────────
//
// A `.lua` config has no `include = [...]` to walk, so the session cannot
// pre-resolve its fragments: it discovers them by running the script against a
// fetcher backed by the project. These cases pin that the discovery is real
// (fragments the walk never enqueued still arrive) and that a fragment the
// project does not have lands in the same WaitingForUser state a missing TOML
// include does, rather than failing the build outright.

TEST_CASE("BuildSession resolves a Lua config's computed includes", "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.lua", stringBytes(R"LUA(
config { deduplicate_shapes = true }
include("materials.lua")
for i = 1, 3 do
  rule { match = ('path ~= "**/Layer%d*"'):format(i), material = "silicon" }
end
)LUA"));
    // Reachable only by running the script — nothing enqueued this key.
    bag->addBytes("materials.lua", stringBytes("material(\"silicon\", { metallic = 1.0 })\n"));
    auto geom = minimalNhbZstBytes();
    bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

    BuildSession session;
    session.setRootKeys("scene.lua", "scene.nhb.zst");
    refreshUntilSettled(session, *bag);

    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
    auto inputs = session.takeInputs();
    REQUIRE(inputs);
    REQUIRE_FALSE(inputs->config.diags.hasErrors());
    REQUIRE(inputs->config.config.deduplicateShapes);
    REQUIRE(inputs->config.config.materials.size() == 1);
    REQUIRE(inputs->config.config.rules.size() == 3);
}

TEST_CASE("BuildSession reports a Lua fragment the project does not have",
          "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.lua", stringBytes("include(\"absent.lua\")\n"));
    auto geom = minimalNhbZstBytes();
    bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

    BuildSession session;
    session.setRootKeys("scene.lua", "scene.nhb.zst");
    refreshUntilSettled(session, *bag);

    // Waiting, not Error: the user can still drop the fragment in, exactly as
    // for a missing TOML include.
    REQUIRE(session.phase() == BuildPhase::WaitingForUser);
    const auto missing = session.missing();
    REQUIRE(std::ranges::find(missing, "absent.lua") != missing.end());

    // Supplying it lets the same session finish, which is what makes re-running
    // the script the right recovery rather than a restart. Polled explicitly:
    // `refreshUntilSettled` treats WaitingForUser as settled and would never ask
    // the session again, which is exactly what the App's frame loop does not do.
    bag->addBytes("absent.lua", stringBytes("material(\"m\", {})\n"));
    for (int i = 0; i < kPollBudget && session.phase() != BuildPhase::ResolvedReady; ++i) {
        session.refresh(*bag);
    }
    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
    auto inputs = session.takeInputs();
    REQUIRE(inputs);
    REQUIRE(inputs->config.config.materials.size() == 1);
}

TEST_CASE("BuildSession input_hash covers a Lua config's fragments", "[viewer][build_session]") {
    // The fragments are what the script's output actually depends on, and the
    // walk never enqueued them — so if they miss the hash, editing a fragment
    // yields a hash identical to the previous build's, which BuildController
    // reads as "same content" and answers by keeping the scene it already has.
    auto geom = minimalNhbZstBytes();

    const auto hashFor = [&](std::string_view fragment) {
        auto bag = std::make_unique<BagProjectFs>();
        bag->addBytes("scene.lua", stringBytes("include(\"materials.lua\")\n"));
        bag->addBytes("materials.lua", stringBytes(fragment));
        bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

        BuildSession session;
        session.setRootKeys("scene.lua", "scene.nhb.zst");
        refreshUntilSettled(session, *bag);
        REQUIRE(session.phase() == BuildPhase::ResolvedReady);
        auto inputs = session.takeInputs();
        REQUIRE(inputs);
        return inputs->input_hash;
    };

    const auto a = hashFor("material(\"silicon\", { metallic = 1.0 })\n");
    const auto b = hashFor("material(\"silicon\", { metallic = 0.0 })\n");
    REQUIRE(a != b);

    // ...and unchanged content still hashes the same, or every poll would look
    // like an edit.
    REQUIRE(hashFor("material(\"silicon\", { metallic = 1.0 })\n") == a);
}

// ── Characterization: what the walk resolves, and what it reports missing ────
//
// These pin behaviour that is about to be re-implemented (the two-phase walk
// collapses into a single fetcher-driven pass), so they are written against the
// *current* code and must survive the change untouched. Neither describes the
// refactor; both describe what a user would notice if it went wrong.

TEST_CASE("BuildSession input_hash covers exactly the TOML include closure",
          "[viewer][build_session]") {
    // `input_hash` decides whether BuildController rebuilds or keeps the live
    // scene, so its *key set* is the thing to pin — too few and an edit is
    // silently ignored, too many and every unrelated file churns the scene.
    //
    // Pinned by comparison rather than by a golden literal: the geometry blob is
    // zstd-compressed, so a literal would also be asserting the compressor's
    // output and would break on a dependency bump for a reason that has nothing
    // to do with this property.
    auto geom = minimalNhbZstBytes();

    struct Project {
        std::string deep;  // contents of the transitively-included file
        bool extra{false}; // an unreferenced sibling in the same bag
    };

    const auto hashFor = [&](const Project &p) {
        auto bag = std::make_unique<BagProjectFs>();
        bag->addBytes("scene.toml", stringBytes("include = [\"mid.toml\"]\n"));
        bag->addBytes("mid.toml", stringBytes("include = [\"deep.toml\"]\n"));
        bag->addBytes("deep.toml", stringBytes(p.deep));
        if (p.extra) {
            bag->addBytes("unreferenced.toml", stringBytes("hoist_orphans = true\n"));
        }
        bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

        BuildSession session;
        session.setRootKeys("scene.toml", "scene.nhb.zst");
        refreshUntilSettled(session, *bag);
        REQUIRE(session.phase() == BuildPhase::ResolvedReady);
        auto inputs = session.takeInputs();
        REQUIRE(inputs);
        return inputs->input_hash;
    };

    const auto base = hashFor({.deep = "deduplicate_shapes = true\n"});

    // Stable: identical content must hash identically, or every poll looks like
    // an edit and nothing is ever cached.
    REQUIRE(hashFor({.deep = "deduplicate_shapes = true\n"}) == base);

    // Sensitive two levels down. This is the dangerous direction: if a
    // transitively-included file falls out of the hash, editing it leaves the
    // viewer showing the old scene with no indication anything was ignored.
    REQUIRE(hashFor({.deep = "deduplicate_shapes = false\n"}) != base);

    // ...and no wider than the closure. A file sitting in the project that the
    // config never includes must not enter the hash, or unrelated drops would
    // invalidate a perfectly good scene.
    REQUIRE(hashFor({.deep = "deduplicate_shapes = true\n", .extra = true}) == base);
}

TEST_CASE("BuildSession names every missing TOML include in one pass", "[viewer][build_session]") {
    // The UI prints `missing()` as a to-do list, so reporting them one at a time
    // would turn one round trip into three. Both siblings are missing and both
    // have to be named; the walk keeps going past the first.
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("include = [\"a.toml\", \"b.toml\"]\n"));
    auto geom = minimalNhbZstBytes();
    bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");
    refreshUntilSettled(session, *bag);

    REQUIRE(session.phase() == BuildPhase::WaitingForUser);
    const auto missing = session.missing();
    REQUIRE(std::ranges::find(missing, "a.toml") != missing.end());
    REQUIRE(std::ranges::find(missing, "b.toml") != missing.end());

    // Not an error, and recoverable in place: supplying both lets the same
    // session finish rather than requiring a restart.
    bag->addBytes("a.toml", stringBytes("hoist_orphans = true\n"));
    bag->addBytes("b.toml", stringBytes("deduplicate_shapes = false\n"));
    for (int i = 0; i < kPollBudget && session.phase() != BuildPhase::ResolvedReady; ++i) {
        session.refresh(*bag);
    }
    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
}

TEST_CASE("BuildSession reports a missing root geometry, not just a missing config",
          "[viewer][build_session]") {
    // The two roots are resolved by the same walk as the includes today. When
    // that walk goes away they still have to be resolved by *something*, and a
    // missing geometry has to stay a WaitingForUser rather than becoming a parse
    // error or an internal one.
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("deduplicate_shapes = true\n"));
    // no scene.nhb.zst

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");
    refreshUntilSettled(session, *bag);

    REQUIRE(session.phase() == BuildPhase::WaitingForUser);
    const auto missing = session.missing();
    REQUIRE(std::ranges::find(missing, "scene.nhb.zst") != missing.end());
}

TEST_CASE("BuildSession names a missing geometry and a missing include together",
          "[viewer][build_session]") {
    // The two halves of the to-do list come from different places — the roots
    // are resolved up front, the includes only once the config has been read —
    // and a user with an incomplete project has both. Naming the geometry
    // alone, taking the file, and only then admitting to the include is one
    // round trip per layer, for a project that was always one drop short.
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("include = [\"a.toml\"]\n"));
    // no scene.nhb.zst, no a.toml

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");
    refreshUntilSettled(session, *bag);

    REQUIRE(session.phase() == BuildPhase::WaitingForUser);
    const auto missing = session.missing();
    REQUIRE(std::ranges::find(missing, "scene.nhb.zst") != missing.end());
    REQUIRE(std::ranges::find(missing, "a.toml") != missing.end());

    // And supplying both at once finishes it, rather than the second one
    // uncovering a third thing to ask for.
    bag->addBytes("a.toml", stringBytes("hoist_orphans = true\n"));
    auto geom = minimalNhbZstBytes();
    bag->addBytes("scene.nhb.zst", std::span<const std::byte>{geom});
    for (int i = 0; i < kPollBudget && session.phase() != BuildPhase::ResolvedReady; ++i) {
        session.refresh(*bag);
    }
    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
}
