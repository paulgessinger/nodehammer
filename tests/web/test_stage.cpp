// Assembling the directory the server serves.
//
// The assertion that matters most is the smallest: an `nh_manifest.json` next to
// the shell is the *entire* difference between the viewer coming up as a locked
// publication and coming up as the empty application. Nothing else in a staged
// root selects a posture, which means a stale sidecar left behind by a previous
// stage silently changes what the next one is.

#include "web/stage.hpp"

#include <nodehammer/diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace web = nodehammer::web;

namespace {

class TempDir {
  public:
    TempDir() : path_(fs::temp_directory_path() / uniqueName()) { fs::create_directories(path_); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const fs::path &path() const { return path_; }

    fs::path write(std::string_view name, std::string_view content) const {
        const fs::path target = path_ / name;
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary);
        out << content;
        return target;
    }

  private:
    static std::string uniqueName() {
        static int n = 0;
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::format("nh_stage_test_{}_{}", tick, n++);
    }
    fs::path path_;
};

/// A runtime directory shaped like an install tree's, minus the megabytes.
void makeRuntime(const TempDir &dir) {
    dir.write("index.html", "<!doctype html>");
    dir.write("compute_worker.js", "// worker");
    dir.write("nodehammer-gles3.js", "// loader");
    dir.write("nodehammer-gles3.wasm", "fake");
    dir.write("nh_runtime.json", "{\"schema\": 1, \"version\": \"test\"}");
}

std::string readAll(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("with nothing to open, the staged root is the application", "[web][stage]") {
    TempDir runtime;
    TempDir target;
    makeRuntime(runtime);

    const web::StagedRoot staged =
        web::stageRoot({.runtime = runtime.path(), .target = target.path()});

    CHECK(staged.posture == web::Posture::Application);
    CHECK(staged.archive.empty());
    // The absence *is* the posture.
    CHECK_FALSE(fs::exists(target.path() / "nh_manifest.json"));
    // And the runtime came across whole.
    for (const auto *name : {"index.html", "compute_worker.js", "nodehammer-gles3.js",
                             "nodehammer-gles3.wasm", "nh_runtime.json"}) {
        CHECK(fs::is_regular_file(target.path() / name));
    }
}

TEST_CASE("an archive makes it a publication", "[web][stage]") {
    TempDir runtime;
    TempDir target;
    TempDir source;
    makeRuntime(runtime);
    const fs::path archive = source.write("my-detector.nhproj", "PK not really");

    const web::StagedRoot staged =
        web::stageRoot({.runtime = runtime.path(), .target = target.path(), .project = archive});

    CHECK(staged.posture == web::Posture::Viewer);
    CHECK(staged.archive == "my-detector.nhproj");
    CHECK(readAll(target.path() / "my-detector.nhproj") == "PK not really");

    const std::string sidecar = readAll(target.path() / "nh_manifest.json");
    CHECK_THAT(sidecar, Catch::Matchers::ContainsSubstring("\"archive\": \"my-detector.nhproj\""));
    CHECK_THAT(sidecar, Catch::Matchers::ContainsSubstring("\"lock\": true"));
    CHECK_THAT(sidecar, Catch::Matchers::ContainsSubstring("my-detector.nhproj\""));
}

TEST_CASE("a title overrides the derived one", "[web][stage]") {
    TempDir runtime, target, source;
    makeRuntime(runtime);
    const fs::path archive = source.write("p.nhproj", "PK");

    const web::StagedRoot staged = web::stageRoot({.runtime = runtime.path(),
                                                   .target = target.path(),
                                                   .project = archive,
                                                   .title = "ODD, tracker only"});
    (void)staged;
    CHECK_THAT(readAll(target.path() / "nh_manifest.json"),
               Catch::Matchers::ContainsSubstring("\"title\": \"ODD, tracker only\""));
}

TEST_CASE("re-staging without a project drops the previous sidecar", "[web][stage]") {
    // The failure this prevents is silent and total: a root that still claims a
    // publication it no longer contains comes up in viewer mode and fails to
    // fetch an archive that is not there, which reads as a broken viewer.
    TempDir runtime, target, source;
    makeRuntime(runtime);
    const fs::path archive = source.write("p.nhproj", "PK");

    static_cast<void>(
        web::stageRoot({.runtime = runtime.path(), .target = target.path(), .project = archive}));
    REQUIRE(fs::exists(target.path() / "nh_manifest.json"));

    const web::StagedRoot again =
        web::stageRoot({.runtime = runtime.path(), .target = target.path()});
    CHECK(again.posture == web::Posture::Application);
    CHECK_FALSE(fs::exists(target.path() / "nh_manifest.json"));
}

TEST_CASE("loose files need both halves", "[web][stage]") {
    TempDir runtime, target, source;
    makeRuntime(runtime);
    const fs::path config = source.write("scene.toml", "[project]\n");

    CHECK_THROWS_AS(
        web::stageRoot({.runtime = runtime.path(), .target = target.path(), .config = config}),
        nodehammer::Error);
}

TEST_CASE("what is not there is named, not guessed at", "[web][stage]") {
    TempDir runtime, target;
    makeRuntime(runtime);

    SECTION("a missing archive") {
        CHECK_THROWS_AS(web::stageRoot({.runtime = runtime.path(),
                                        .target = target.path(),
                                        .project = target.path() / "absent.nhproj"}),
                        nodehammer::Error);
    }
    SECTION("a missing runtime") {
        CHECK_THROWS_AS(
            web::stageRoot({.runtime = runtime.path() / "absent", .target = target.path()}),
            nodehammer::Error);
    }
}
