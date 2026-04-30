#include <catch2/catch_test_macros.hpp>

#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/ir/fb/semantic/flatbuffer.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/viewer/bag_project_fs.hpp>
#include <nodehammer/viewer/build_session.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace nodehammer;
using nodehammer::viewer::BagProjectFs;
using nodehammer::viewer::BuildPhase;
using nodehammer::viewer::BuildSession;

namespace {

constexpr int kPollBudget = 100;

SemanticScene makeMinimalScene() {
    SemanticScene scene;

    auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, BoxShape{5.0, 10.0, 15.0}};

    auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "iron", glm::vec3{0.5f, 0.5f, 0.5f}, 7.87};

    auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "ironBox", shapeId, matId};

    auto nodeId = scene.nextNodeId();
    SemanticNode node;
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
    return zstd_io::compress(std::span<const std::byte>{raw});
}

std::vector<std::byte> stringBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

void pollUntilSettled(BuildSession &session, viewer::ProjectFs &project) {
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
        session.poll(&project);
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
        session.poll(bag.get());
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

TEST_CASE("BuildSession surfaces missing geometry as WaitingForUser", "[viewer][build_session]") {
    auto bag = std::make_unique<BagProjectFs>();
    bag->addBytes("scene.toml", stringBytes("# minimal\n"));

    BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    pollUntilSettled(session, *bag);

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
        session.poll(bag.get());
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
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    for (int i = 0; i < kPollBudget; ++i) {
        session.poll(bag.get());
        if (session.phase() == BuildPhase::Error || session.phase() == BuildPhase::ResolvedReady) {
            break;
        }
    }

    REQUIRE(session.phase() == BuildPhase::Error);
    REQUIRE_FALSE(session.errorMessage().empty());
}
