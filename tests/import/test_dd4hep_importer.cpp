#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/dd4hep/semantic/importer.hpp>
#include <nodehammer/ir/semantic.hpp>

#include <string>
#include <unordered_set>

// Fixture paths injected at compile time from CMake
#ifndef NODEHAMMER_FIXTURES_DIR
#define NODEHAMMER_FIXTURES_DIR "fixtures"
#endif

static const std::string kSimpleBox =
    std::string{NODEHAMMER_FIXTURES_DIR} + "/dd4hep/simple_box.xml";
static const std::string kSensitiveDet =
    std::string{NODEHAMMER_FIXTURES_DIR} + "/dd4hep/sensitive_detector.xml";

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("DD4hepImporter: formatName and supportedExtensions", "[import][dd4hep]") {
    nodehammer::DD4hepImporter imp;
    REQUIRE(imp.formatName() == "dd4hep");
    REQUIRE(imp.supportedExtensions().empty());
}

TEST_CASE("DD4hepImporter: simple_box.xml -> no errors, sourceSystem dd4hep", "[import][dd4hep]") {
    nodehammer::DD4hepImporter imp;
    auto result = imp.import(kSimpleBox);

    REQUIRE_FALSE(result.diags.hasErrors());
    REQUIRE_FALSE(result.scene.nodes.empty());

    const auto &root = result.scene.nodes.at(result.scene.rootId);
    REQUIRE(root.sourceSystem == "dd4hep");
}

TEST_CASE("DD4hepImporter: simple_box.xml -> scene contains a BoxShape", "[import][dd4hep]") {
    nodehammer::DD4hepImporter imp;
    auto result = imp.import(kSimpleBox);

    bool hasBox = false;
    for (const auto &[id, s] : result.scene.shapes) {
        if (std::holds_alternative<nodehammer::BoxShape>(s.data)) {
            hasBox = true;
            break;
        }
    }
    REQUIRE(hasBox);
}

TEST_CASE("DD4hepImporter: simple_box.xml -> subdetector tag propagated", "[import][dd4hep]") {
    nodehammer::DD4hepImporter imp;
    auto result = imp.import(kSimpleBox);

    // At least one node should carry the "subdetector" tag
    bool hasTag = false;
    for (const auto &[id, node] : result.scene.nodes) {
        if (node.tags.count("subdetector")) {
            hasTag = true;
            break;
        }
    }
    REQUIRE(hasTag);
}

TEST_CASE("DD4hepImporter: sensitive_detector.xml -> sensitive tag set", "[import][dd4hep]") {
    nodehammer::DD4hepImporter imp;
    auto result = imp.import(kSensitiveDet);

    REQUIRE_FALSE(result.diags.hasErrors());

    bool hasSensitive = false;
    for (const auto &[id, node] : result.scene.nodes) {
        auto it = node.tags.find("sensitive");
        if (it != node.tags.end() && it->second == "true") {
            hasSensitive = true;
            break;
        }
    }
    REQUIRE(hasSensitive);
}

TEST_CASE("DD4hepImporter: all nodes reachable from root via BFS", "[import][dd4hep]") {
    nodehammer::DD4hepImporter imp;
    auto result = imp.import(kSimpleBox);

    std::unordered_set<nodehammer::SemanticNodeId> visited;
    result.scene.visitBFS([&](const auto &node) { visited.insert(node.id); });
    REQUIRE(visited.size() == result.scene.nodes.size());
}
