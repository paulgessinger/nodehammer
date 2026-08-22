// `inspect --output-format json`.
//
// `inspect` answers questions *about* a scene — how many nodes, which tags,
// what is under this path — where `convert` translates the whole thing. That
// distinction is why the projections need a machine format of their own: a
// caller asking "what tags does this detector carry" does not want the entire
// semantic IR as JSON, it wants the tag index, and until now the only way to
// get one was to parse a rendering meant for a terminal.
//
// Two properties are asserted here that a "same content, other skin" reading
// would miss, because the JSON is deliberately *not* the text view re-encoded:
//
//   - the tag index is complete, where the text view samples five values once a
//     key passes ten — a kindness to a reader and a lie to a parser;
//   - `--output-format` does not follow the TTY. Colour and paging may flip on a pipe
//     because they are presentation; document structure may not, or a pipe
//     added later silently breaks whatever was reading it.

#include "cli_test_support.hpp"

#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/semantic.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace {

/// A scene with two tagged children, so `tags` and `tree` have something to say.
nodehammer::ir::semantic::Scene makeTaggedScene() {
    using namespace nodehammer::ir;
    semantic::Scene scene;

    const auto shapeId = scene.nextShapeId();
    scene.shapes[shapeId] = {shapeId, semantic::BoxShape{1.0, 2.0, 3.0}};
    const auto matId = scene.nextMaterialId();
    scene.materials[matId] = {matId, "iron", glm::vec3{0.5f, 0.5f, 0.5f}, 7.87};
    const auto lvId = scene.nextLogVolId();
    scene.logVols[lvId] = {lvId, "box", shapeId, matId};

    const auto rootId = scene.nextNodeId();
    semantic::Node root;
    root.id = rootId;
    root.name = "world";
    root.logVolId = lvId;
    scene.nodes[rootId] = root;
    scene.rootId = rootId;

    for (const auto *name : {"tracker", "calo"}) {
        const auto childId = scene.nextNodeId();
        semantic::Node child;
        child.id = childId;
        child.name = name;
        child.logVolId = lvId;
        child.parentId = rootId;
        child.tags["subdetector"] = name;
        scene.nodes[childId] = child;
        scene.nodes[rootId].children.push_back(childId);
    }

    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return scene;
}

/// The scene on disk as `.nhb`, removed when the test ends.
class SceneFile {
  public:
    SceneFile() {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / std::format("nh_inspect_{}.nhb", tick);
        const auto bytes = nodehammer::ir::semanticSceneToBytes(makeTaggedScene());
        std::ofstream out(path_, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ~SceneFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    SceneFile(const SceneFile &) = delete;
    SceneFile &operator=(const SceneFile &) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

  private:
    fs::path path_;
};

/// Arbitrary text on disk, removed when the test ends.
class TextFile {
  public:
    TextFile(std::string stem, std::string_view content, std::string_view extension) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() / std::format("nh_{}_{}{}", stem, tick, extension);
        std::ofstream out(path_);
        out << content;
    }
    ~TextFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    TextFile(const TextFile &) = delete;
    TextFile &operator=(const TextFile &) = delete;

    [[nodiscard]] std::string path() const { return path_.string(); }

  private:
    fs::path path_;
};

nlohmann::json parsed(const nhtest::Outcome &outcome) {
    INFO("stdout was: " << outcome.out);
    REQUIRE(outcome.code == 0);
    return nlohmann::json::parse(outcome.out);
}

} // namespace

TEST_CASE("inspect summary answers in JSON", "[cli][inspect][json]") {
    const SceneFile scene;
    const auto doc = parsed(
        nhtest::runCaptured({"inspect", "-i", scene.path(), "--output-format", "json", "summary"}));

    CHECK(doc["schema"] == 1);
    CHECK(doc["kind"] == "summary");
    CHECK(doc["nodes"] == 3);
    CHECK(doc["shapes"]["box"] == 1);
    CHECK(doc["materials"] == nlohmann::json::array({"iron"}));
    CHECK(doc["diagnostics"]["errors"] == 0);
}

TEST_CASE("inspect tree answers in JSON, flat and filterable", "[cli][inspect][json]") {
    const SceneFile scene;

    SECTION("the whole tree") {
        const auto doc = parsed(nhtest::runCaptured(
            {"inspect", "-i", scene.path(), "--output-format", "json", "tree"}));
        CHECK(doc["kind"] == "tree");
        CHECK(doc["shown"] == 3);
        CHECK(doc["filtered"] == 0);
        // Absent limits are present-and-null, so a caller reads the field
        // unconditionally rather than testing for its existence.
        CHECK(doc["maxDepth"].is_null());
        CHECK(doc["filter"].is_null());
        CHECK(doc["nodes"].size() == 3);
        CHECK(doc["nodes"][0]["depth"] == 0);
        CHECK(doc["nodes"][0]["leaf"] == false);
    }

    SECTION("a filter reports both halves of the count") {
        const auto doc =
            parsed(nhtest::runCaptured({"inspect", "-i", scene.path(), "--output-format", "json",
                                        "tree", "--filter", "/world/calo"}));
        CHECK(doc["shown"] == 1);
        CHECK(doc["filtered"] == 2);
        CHECK(doc["filter"] == "/world/calo");
        CHECK(doc["nodes"][0]["name"] == "calo");
        CHECK(doc["nodes"][0]["tags"]["subdetector"] == "calo");
    }

    SECTION("a depth limit is reported, not merely applied") {
        const auto doc = parsed(nhtest::runCaptured(
            {"inspect", "-i", scene.path(), "--output-format", "json", "tree", "--depth", "0"}));
        CHECK(doc["maxDepth"] == 0);
        CHECK(doc["shown"] == 1);
        CHECK(doc["nodes"][0]["children"] == 2);
    }
}

TEST_CASE("an empty tree is still a tree document", "[cli][inspect][json]") {
    // A scene with nothing to walk leaves the renderer by a different door, and
    // the shape of the document may not change with it: a caller that reads
    // `maxDepth` and `filter` unconditionally would otherwise fail on exactly
    // the scenes it has least reason to special-case. The empty scene comes in
    // as JSON because the binary writer refuses to name a root that isn't there.
    const TextFile scene{"nhjson", R"({"header": {"version": 1, "type": "semantic"},
                                       "content": {"rootId": 0, "nodes": [], "logVols": [],
                                                   "shapes": [], "materials": []}})",
                         ".json"};
    const auto doc =
        parsed(nhtest::runCaptured({"inspect", "-i", scene.path(), "--output-format", "json",
                                    "tree", "--depth", "2", "--filter", "/world/*"}));

    CHECK(doc["kind"] == "tree");
    CHECK(doc["shown"] == 0);
    CHECK(doc["filtered"] == 0);
    CHECK(doc["nodes"] == nlohmann::json::array());
    CHECK(doc["maxDepth"] == 2);
    CHECK(doc["filter"] == "/world/*");
}

TEST_CASE("inspect tags answers in JSON, with every value", "[cli][inspect][json]") {
    const SceneFile scene;
    const auto doc = parsed(
        nhtest::runCaptured({"inspect", "-i", scene.path(), "--output-format", "json", "tags"}));

    CHECK(doc["kind"] == "tags");
    CHECK(doc["nodeCount"] == 3);
    CHECK(doc["nodesWithTags"] == 2);
    CHECK(doc["keys"]["subdetector"] == nlohmann::json::array({"calo", "tracker"}));
}

TEST_CASE("text is the default, and the format is not inferred from the TTY",
          "[cli][inspect][json]") {
    // The whole point of defaulting to text rather than detecting: this suite
    // runs with stdout redirected to a file, which is exactly the condition a
    // TTY check would read as "not a person, emit JSON".
    const SceneFile scene;
    const auto outcome = nhtest::runCaptured({"inspect", "-i", scene.path(), "summary"});

    REQUIRE(outcome.code == 0);
    CHECK(outcome.out.find("Format:") != std::string::npos);
    CHECK(outcome.out.find("\"schema\"") == std::string::npos);
}

TEST_CASE("an unknown format is refused at parse", "[cli][inspect][json]") {
    const SceneFile scene;
    const auto outcome =
        nhtest::runCaptured({"inspect", "-i", scene.path(), "--output-format", "yaml", "summary"});

    CHECK(outcome.code != 0);
    CHECK(outcome.mentions("yaml"));
}
