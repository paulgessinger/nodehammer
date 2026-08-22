// Assembling a `.nhproj` from loose files.
//
// Two properties carry the weight here, and neither is about the zip format.
//
// The first is that the *mount* decides the archive's key space. Keys are the
// archive's public surface — an include resolves against the key of the file
// that named it — so a pack that picks a different root produces an archive
// that is byte-for-byte plausible and opens empty. That failure has no symptom
// on this machine; it appears in a browser, later, as a build error.
//
// The second is that identical inputs produce identical bytes. `publish` names
// the archive `project.<hash>.nhproj` so that republishing cache-busts and an
// unchanged publication keeps its URL; a hash that moves on its own makes both
// halves of that false.

#include "project/pack.hpp"

#include <viewer/project_manifest.hpp>
#include <viewer/zip_working_set.hpp>

#include <nodehammer/diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace project = nodehammer::project;
namespace viewer = nodehammer::viewer;

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
        return std::format("nh_pack_test_{}_{}", tick, n++);
    }
    fs::path path_;
};

/// A `.nhb.zst` in name only. Nothing in `pack` reads a blob it embeds — it is
/// copied through the working set verbatim — so the bytes need only be
/// distinguishable.
fs::path fakeBlob(const TempDir &dir, std::string_view name) {
    return dir.write(name, "NHB-not-really");
}

viewer::ZipWorkingSet openPacked(const project::PackResult &packed) {
    return viewer::ZipWorkingSet::openFromBytes(packed.bytes);
}

std::string entryText(viewer::ZipWorkingSet &ws, std::string_view key) {
    const auto bytes = ws.read(key);
    REQUIRE(bytes.has_value());
    const auto sp = bytes->span();
    return std::string(reinterpret_cast<const char *>(sp.data()), sp.size());
}

} // namespace

TEST_CASE("a packed archive names its own entry points", "[project][pack]") {
    TempDir src;
    const fs::path config = src.write("scene.toml", "[project]\n");
    const fs::path blob = fakeBlob(src, "odd.nhb.zst");

    const auto packed = project::pack({.config = config, .geometry = blob});

    CHECK(packed.configKey == "scene.toml");
    CHECK(packed.geometryKey == "odd.nhb.zst");
    CHECK_FALSE(packed.imported);

    auto ws = openPacked(packed);
    const auto manifest = viewer::parseProjectManifest(ws.read("nodehammer.toml")->span());
    REQUIRE(manifest.has_value());
    CHECK(manifest->config_key == "scene.toml");
    CHECK(manifest->geometry_key == "odd.nhb.zst");
    CHECK(entryText(ws, "odd.nhb.zst") == "NHB-not-really");
}

TEST_CASE("the include chain comes along, with its keys intact", "[project][pack]") {
    // The keys are the point. `scene.toml` says `include = "common/base.toml"`,
    // so the archive has to hold that exact key for the include to resolve
    // again on the other side.
    TempDir src;
    src.write("common/base.toml", "[render]\n");
    const fs::path config = src.write("scene.toml", "include = \"common/base.toml\"\n");
    const fs::path blob = fakeBlob(src, "odd.nhb.zst");

    const auto packed = project::pack({.config = config, .geometry = blob});
    auto ws = openPacked(packed);

    CHECK(ws.read("common/base.toml").has_value());
    CHECK(ws.read("scene.toml").has_value());
}

TEST_CASE("the mount reaches both inputs when they are apart", "[project][pack]") {
    TempDir src;
    const fs::path config = src.write("configs/scene.toml", "[project]\n");
    const fs::path blob = fakeBlob(src, "blobs/odd.nhb.zst");

    const auto packed = project::pack({.config = config, .geometry = blob});

    // The deepest directory holding both, so the keys keep their relative shape
    // rather than collapsing to two bare filenames that no longer say where
    // they came from.
    CHECK(packed.root == fs::canonical(src.path()));
    CHECK(packed.configKey == "configs/scene.toml");
    CHECK(packed.geometryKey == "blobs/odd.nhb.zst");
}

TEST_CASE("an explicit root overrides the derived one", "[project][pack]") {
    TempDir src;
    const fs::path config = src.write("configs/scene.toml", "[project]\n");
    const fs::path blob = fakeBlob(src, "configs/odd.nhb.zst");

    const auto derived = project::pack({.config = config, .geometry = blob});
    CHECK(derived.configKey == "scene.toml");

    const auto rooted = project::pack({.config = config, .geometry = blob, .root = src.path()});
    CHECK(rooted.configKey == "configs/scene.toml");
    CHECK(rooted.geometryKey == "configs/odd.nhb.zst");
}

TEST_CASE("a root that does not contain an input is refused", "[project][pack]") {
    // Rather than answering with a `../` key: the archive would be written, and
    // would open with nothing in it.
    TempDir src;
    TempDir elsewhere;
    const fs::path config = src.write("scene.toml", "[project]\n");
    const fs::path blob = fakeBlob(src, "odd.nhb.zst");

    CHECK_THROWS_AS(project::pack({.config = config, .geometry = blob, .root = elsewhere.path()}),
                    nodehammer::Error);
}

TEST_CASE("what is not there is named, not guessed at", "[project][pack]") {
    TempDir src;
    const fs::path config = src.write("scene.toml", "[project]\n");
    const fs::path blob = fakeBlob(src, "odd.nhb.zst");

    SECTION("a missing config") {
        CHECK_THROWS_AS(project::pack({.config = src.path() / "absent.toml", .geometry = blob}),
                        nodehammer::Error);
    }
    SECTION("a missing input") {
        CHECK_THROWS_AS(project::pack({.config = config, .geometry = src.path() / "absent.nhb"}),
                        nodehammer::Error);
    }
    SECTION("only one of the two") {
        CHECK_THROWS_AS(project::pack({.config = config}), nodehammer::Error);
        CHECK_THROWS_AS(project::pack({.geometry = blob}), nodehammer::Error);
    }
    SECTION("one file asked to be both") {
        CHECK_THROWS_AS(project::pack({.config = config, .geometry = config}), nodehammer::Error);
    }
}

TEST_CASE("an unresolvable include refuses rather than packing a partial scene",
          "[project][pack]") {
    // The archive would open, and fail to build, on a machine that cannot say
    // which file was missing here.
    TempDir src;
    const fs::path config = src.write("scene.toml", "include = \"nowhere/base.toml\"\n");
    const fs::path blob = fakeBlob(src, "odd.nhb.zst");

    CHECK_THROWS_AS(project::pack({.config = config, .geometry = blob}), nodehammer::Error);
}

TEST_CASE("identical inputs pack to identical bytes", "[project][pack]") {
    // What `publish`'s content-hashed archive name rests on. Two serializations
    // of one working set used to differ in both entry order (an unordered_map
    // iterated into the writer) and mtime (miniz stamps the current time).
    TempDir src;
    src.write("common/base.toml", "[render]\n");
    const fs::path config = src.write("scene.toml", "include = \"common/base.toml\"\n");
    const fs::path blob = fakeBlob(src, "odd.nhb.zst");

    const auto first = project::pack({.config = config, .geometry = blob});
    const auto second = project::pack({.config = config, .geometry = blob});

    CHECK(first.bytes == second.bytes);
}
