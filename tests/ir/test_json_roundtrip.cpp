#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render_json.hpp>
#include <nodehammer/ir/semantic_json.hpp>

TEST_CASE("to_json: BoxShape serialization", "[ir][json]") {
    nodehammer::BoxShape box{1.0, 2.0, 3.0};
    nlohmann::json j = box;
    REQUIRE(j["type"] == "box");
    REQUIRE(j["dx"] == Catch::Approx(1.0));
    REQUIRE(j["dy"] == Catch::Approx(2.0));
    REQUIRE(j["dz"] == Catch::Approx(3.0));
}

TEST_CASE("to_json: SemanticNode serialization", "[ir][json]") {
    nodehammer::SemanticNode node;
    node.id = nodehammer::SemanticNodeId{42};
    node.name = "testNode";
    node.logVolId = nodehammer::SemanticLogVolId{7};
    node.tags["subdetector"] = "tracker";
    node.sourceSystem = "tgeo";

    nlohmann::json j = node;
    REQUIRE(j["id"] == 42);
    REQUIRE(j["name"] == "testNode");
    REQUIRE(j["logVolId"] == 7);
    REQUIRE(j["tags"]["subdetector"] == "tracker");
    REQUIRE(j["sourceSystem"] == "tgeo");
}

TEST_CASE("to_json: RenderMaterial serialization", "[ir][json]") {
    nodehammer::RenderMaterial mat;
    mat.id = nodehammer::RenderMaterialId{1};
    mat.name = "steel";
    mat.baseColorFactor = glm::vec4{0.5f, 0.5f, 0.5f, 1.f};
    mat.metallicFactor = 0.8f;
    mat.roughnessFactor = 0.2f;

    nlohmann::json j = mat;
    REQUIRE(j["name"] == "steel");
    REQUIRE(j["metallicFactor"] == Catch::Approx(0.8f));
    REQUIRE(j["roughnessFactor"] == Catch::Approx(0.2f));
    REQUIRE(j["baseColorFactor"][0] == Catch::Approx(0.5f));
}

TEST_CASE("to_json: MeshAsset serialization", "[ir][json]") {
    nodehammer::detail::MeshAsset asset;
    asset.id = nodehammer::MeshAssetId{3};
    asset.name = "boxMesh";
    asset.provenance.sourceSystem = "synthetic";

    // Add a couple of placeholder entries
    asset.vertices.push_back({glm::vec3{1, 0, 0}, glm::vec3{0, 1, 0}});
    asset.indices.push_back(0);
    asset.indices.push_back(1);
    asset.indices.push_back(2);

    nlohmann::json j = asset;
    REQUIRE(j["id"] == 3);
    REQUIRE(j["name"] == "boxMesh");
    REQUIRE(j["vertexCount"] == 1);
    REQUIRE(j["indexCount"] == 3);
    REQUIRE(j["provenance"]["sourceSystem"] == "synthetic");
}

TEST_CASE("to_json: RenderNode extras", "[ir][json]") {
    nodehammer::detail::RenderNode node;
    node.id = nodehammer::RenderNodeId{1};
    node.name = "volume";
    node.semanticNodeId = nodehammer::SemanticNodeId{9};

    SECTION("populated extras are emitted") {
        node.extras = {{"detector", "tracker"}};
        nlohmann::json j = node;
        REQUIRE(j["extras"]["detector"] == "tracker");
    }

    SECTION("null/empty extras are omitted") {
        nlohmann::json j = node;
        REQUIRE_FALSE(j.contains("extras"));
    }
}

TEST_CASE("to_json: TubeShape serialization", "[ir][json]") {
    nodehammer::TubeShape tube{5.0, 10.0, 20.0, 0.0, 3.14159265};
    nlohmann::json j = tube;
    REQUIRE(j["type"] == "tube");
    REQUIRE(j["rMin"] == Catch::Approx(5.0));
    REQUIRE(j["rMax"] == Catch::Approx(10.0));
}

TEST_CASE("to_json: UnknownShape serialization", "[ir][json]") {
    nodehammer::UnknownShape unk{"TGeoArb8"};
    nlohmann::json j = unk;
    REQUIRE(j["type"] == "unknown");
    REQUIRE(j["originalType"] == "TGeoArb8");
}

TEST_CASE("DiagnosticList: hasFatal and hasErrors", "[ir][diagnostics]") {
    nodehammer::DiagnosticList list;
    REQUIRE_FALSE(list.hasFatal());
    REQUIRE_FALSE(list.hasErrors());

    list.warn("NH0301", "just a warning");
    REQUIRE_FALSE(list.hasFatal());
    REQUIRE_FALSE(list.hasErrors());

    list.error("NH0500", "an error");
    REQUIRE_FALSE(list.hasFatal());
    REQUIRE(list.hasErrors());

    list.fatal("NH0503", "fatal");
    REQUIRE(list.hasFatal());
    REQUIRE(list.hasErrors());
}
