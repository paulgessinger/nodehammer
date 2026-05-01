#include <catch2/catch_test_macros.hpp>

#include <nodehammer/viewer/build_session.hpp>
#include <nodehammer/viewer/filesystem_project_fs.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

using nodehammer::viewer::BuildPhase;
using nodehammer::viewer::BuildSession;
using nodehammer::viewer::DirNode;
using nodehammer::viewer::FilesystemProjectFs;
using nodehammer::viewer::ResolveStatus;
using Kind = nodehammer::viewer::ProjectDropDecision::Kind;

namespace {

/// Each test mounts under a unique temp dir so parallel `ctest -j` runs
/// don't collide. The dir is wiped on construction and on destruction —
/// safe because we always live below `temp_directory_path()`.
struct TempProject {
    std::filesystem::path root;

    explicit TempProject(std::string_view tag) {
        root = std::filesystem::temp_directory_path() /
               (std::string{"nh_filesystem_project_fs_"} + std::string{tag});
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    TempProject(const TempProject &) = delete;
    TempProject &operator=(const TempProject &) = delete;

    void writeFile(std::string_view rel, std::string_view contents) const {
        const auto p = root / std::filesystem::path{rel};
        std::filesystem::create_directories(p.parent_path());
        std::ofstream f{p, std::ios::binary};
        f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
};

const DirNode *findChild(std::span<const DirNode> children, std::string_view name) {
    auto it = std::ranges::find_if(children, [&](const DirNode &n) { return n.name == name; });
    return it == children.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("FilesystemProjectFs walks a flat directory and resolves bytes",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"flat"};
    tp.writeFile("scene.toml", "# minimal config\n");
    tp.writeFile("notes.txt", "hello");

    FilesystemProjectFs fs{tp.root};
    REQUIRE(fs.status() == nodehammer::viewer::ProjectFsStatus::Ready);

    auto top = fs.list("");
    REQUIRE(top.size() == 2);
    REQUIRE(findChild(top, "scene.toml") != nullptr);
    REQUIRE(findChild(top, "notes.txt") != nullptr);

    auto r = fs.resolve("scene.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    std::string s{reinterpret_cast<const char *>(r.file.bytes.data()), r.file.bytes.size()};
    REQUIRE(s == "# minimal config\n");
}

TEST_CASE("FilesystemProjectFs resolves nested keys via real subdir paths",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"nested"};
    tp.writeFile("scene.toml", "# parent\n");
    tp.writeFile("subdir/common.toml", "# included\n");

    FilesystemProjectFs fs{tp.root};

    auto top = fs.list("");
    auto *subdir = findChild(top, "subdir");
    REQUIRE(subdir != nullptr);
    REQUIRE(subdir->is_directory);
    REQUIRE(subdir->children.size() == 1);
    REQUIRE(subdir->children.front().key == "subdir/common.toml");

    auto r = fs.resolve("subdir/common.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    std::string s{reinterpret_cast<const char *>(r.file.bytes.data()), r.file.bytes.size()};
    REQUIRE(s == "# included\n");
}

TEST_CASE("FilesystemProjectFs returns Missing for unknown keys",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"missing"};
    tp.writeFile("scene.toml", "x");

    FilesystemProjectFs fs{tp.root};
    auto r = fs.resolve("not-here.toml");
    REQUIRE(r.status == ResolveStatus::Missing);
    REQUIRE(r.missing_key == "not-here.toml");
}

TEST_CASE("FilesystemProjectFs::rescan picks up freshly-written files",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"rescan"};
    tp.writeFile("scene.toml", "x");
    FilesystemProjectFs fs{tp.root};
    const auto gen0 = fs.generation();

    REQUIRE(fs.resolve("subdir/common.toml").status == ResolveStatus::Missing);

    tp.writeFile("subdir/common.toml", "appeared");
    fs.rescan();
    REQUIRE(fs.generation() > gen0);

    auto r = fs.resolve("subdir/common.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    std::string s{reinterpret_cast<const char *>(r.file.bytes.data()), r.file.bytes.size()};
    REQUIRE(s == "appeared");
}

TEST_CASE("FilesystemProjectFs::rescan invalidates cached bytes",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"rescan_invalidate"};
    tp.writeFile("scene.toml", "first");
    FilesystemProjectFs fs{tp.root};

    auto r1 = fs.resolve("scene.toml");
    REQUIRE(r1.status == ResolveStatus::Ready);
    std::string s1{reinterpret_cast<const char *>(r1.file.bytes.data()), r1.file.bytes.size()};
    REQUIRE(s1 == "first");

    tp.writeFile("scene.toml", "second contents");
    fs.rescan();

    auto r2 = fs.resolve("scene.toml");
    REQUIRE(r2.status == ResolveStatus::Ready);
    std::string s2{reinterpret_cast<const char *>(r2.file.bytes.data()), r2.file.bytes.size()};
    REQUIRE(s2 == "second contents");
}

TEST_CASE("FilesystemProjectFs subdir span excludes grandchildren when followed by a sibling",
          "[viewer][filesystem_project_fs]") {
    // Repros the layout bug in pre-order DFS: when `inner` is followed
    // by `sibling.toml` at the same level, DFS places `deep.toml`
    // between them in the flat array, and a contiguous span over
    // subdir's children would erroneously include `deep.toml`.
    TempProject tp{"interleaved"};
    tp.writeFile("subdir/inner/deep.toml", "");
    tp.writeFile("subdir/sibling.toml", "");

    FilesystemProjectFs fs{tp.root};
    auto top = fs.list("");
    REQUIRE(top.size() == 1);
    auto *subdir = findChild(top, "subdir");
    REQUIRE(subdir != nullptr);
    REQUIRE(subdir->children.size() == 2);
    REQUIRE(findChild(subdir->children, "inner") != nullptr);
    REQUIRE(findChild(subdir->children, "sibling.toml") != nullptr);
    REQUIRE(findChild(subdir->children, "deep.toml") == nullptr);
}

TEST_CASE("FilesystemProjectFs root span does not leak nested entries",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"nesting"};
    tp.writeFile("a.toml", "");
    tp.writeFile("subdir/b.toml", "");
    tp.writeFile("subdir/c.toml", "");
    tp.writeFile("subdir/inner/d.toml", "");

    FilesystemProjectFs fs{tp.root};
    auto top = fs.list("");
    REQUIRE(top.size() == 2);
    REQUIRE(findChild(top, "a.toml") != nullptr);
    REQUIRE(findChild(top, "subdir") != nullptr);
    REQUIRE(findChild(top, "b.toml") == nullptr);
    REQUIRE(findChild(top, "c.toml") == nullptr);
    REQUIRE(findChild(top, "d.toml") == nullptr);
    REQUIRE(findChild(top, "inner") == nullptr);

    auto *subdir = findChild(top, "subdir");
    REQUIRE(subdir->children.size() == 3);
    REQUIRE(findChild(subdir->children, "b.toml") != nullptr);
    REQUIRE(findChild(subdir->children, "c.toml") != nullptr);
    REQUIRE(findChild(subdir->children, "inner") != nullptr);
    REQUIRE(findChild(subdir->children, "d.toml") == nullptr);

    auto *inner = findChild(subdir->children, "inner");
    REQUIRE(inner->children.size() == 1);
    REQUIRE(inner->children.front().name == "d.toml");
}

TEST_CASE("FilesystemProjectFs skips dot-prefixed entries by default",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"hidden_default"};
    tp.writeFile("scene.toml", "x");
    tp.writeFile(".DS_Store", "noise");
    tp.writeFile(".git/HEAD", "ref: refs/heads/main");

    FilesystemProjectFs fs{tp.root};
    auto top = fs.list("");
    REQUIRE(top.size() == 1);
    REQUIRE(top.front().name == "scene.toml");

    REQUIRE(fs.resolve(".DS_Store").status == ResolveStatus::Missing);
    REQUIRE(fs.resolve(".git/HEAD").status == ResolveStatus::Missing);
}

TEST_CASE("FilesystemProjectFs includes dot-prefixed entries when option toggled off",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"hidden_opt_in"};
    tp.writeFile("scene.toml", "x");
    tp.writeFile(".secret/foo.toml", "y");

    FilesystemProjectFs fs{tp.root, FilesystemProjectFs::Options{.skip_hidden_files = false}};
    auto top = fs.list("");
    REQUIRE(top.size() == 2);
    REQUIRE(findChild(top, ".secret") != nullptr);
}

TEST_CASE("FilesystemProjectFs::addPath emits a warning rather than mutating",
          "[viewer][filesystem_project_fs]") {
    TempProject tp{"addpath_warning"};
    tp.writeFile("scene.toml", "x");
    FilesystemProjectFs fs{tp.root};

    REQUIRE(fs.planAddPath(std::filesystem::path{"/tmp/somewhere.toml"}).kind == Kind::Reject);
    fs.addPath(std::filesystem::path{"/tmp/somewhere.toml"});
    REQUIRE_FALSE(fs.warnings().empty());
}
