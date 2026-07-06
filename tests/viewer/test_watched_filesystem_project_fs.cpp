#include <catch2/catch_test_macros.hpp>

#include <nodehammer/viewer/filesystem_project_fs.hpp>
#include <nodehammer/viewer/watched_filesystem_project_fs.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
using nodehammer::viewer::DirNode;
using nodehammer::viewer::FilesystemProjectFs;
using nodehammer::viewer::ResolveStatus;
using nodehammer::viewer::WatchedFilesystemProjectFs;

namespace {

/// Unique temp dir per test so parallel `ctest -j` runs don't collide.
/// Wiped on construction and destruction — safe because we always live
/// below `temp_directory_path()`.
struct TempProject {
    std::filesystem::path root;

    explicit TempProject(std::string_view tag) {
        root = std::filesystem::temp_directory_path() /
               (std::string{"nh_watched_fs_project_fs_"} + std::string{tag});
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

/// A zero debounce makes `notifyChanged()` + `poll()` fire synchronously,
/// so the change-coalescing logic is testable without threads or sleeps.
std::unique_ptr<WatchedFilesystemProjectFs> mountNoDebounce(const std::filesystem::path &root) {
    return std::make_unique<WatchedFilesystemProjectFs>(std::make_unique<FilesystemProjectFs>(root),
                                                        0ms);
}

} // namespace

TEST_CASE("WatchedFilesystemProjectFs bumps generation on a debounced change",
          "[viewer][watched_filesystem_project_fs]") {
    TempProject tp{"bump"};
    auto w = mountNoDebounce(tp.root);

    const auto gen0 = w->generation();
    w->notifyChanged();
    w->poll();
    REQUIRE(w->generation() == gen0 + 1);
}

TEST_CASE("WatchedFilesystemProjectFs coalesces a burst into one bump",
          "[viewer][watched_filesystem_project_fs]") {
    TempProject tp{"coalesce"};
    auto w = mountNoDebounce(tp.root);

    const auto gen0 = w->generation();
    w->notifyChanged();
    w->notifyChanged();
    w->notifyChanged();
    w->poll(); // a single rescan despite three notifications
    REQUIRE(w->generation() == gen0 + 1);
}

TEST_CASE("WatchedFilesystemProjectFs leaves generation alone without a change",
          "[viewer][watched_filesystem_project_fs]") {
    TempProject tp{"noop"};
    auto w = mountNoDebounce(tp.root);

    const auto gen0 = w->generation();
    w->poll();
    REQUIRE(w->generation() == gen0);
}

TEST_CASE("WatchedFilesystemProjectFs forwards reads to the inner FS",
          "[viewer][watched_filesystem_project_fs]") {
    TempProject tp{"forward"};
    // Write before mounting so the live watcher has nothing to report —
    // the watcher only sees changes after it starts.
    tp.writeFile("detector.toml", "name = \"odd\"\n");
    auto w = mountNoDebounce(tp.root);

    REQUIRE(w->name() == "filesystem");

    auto root = w->list("");
    const DirNode *node = findChild(root, "detector.toml");
    REQUIRE(node != nullptr);
    REQUIRE_FALSE(node->is_directory);

    auto res = w->resolve("detector.toml");
    REQUIRE(res.status == ResolveStatus::Ready);
    const auto bytes = res.file.bytes.span();
    const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
    REQUIRE(text == "name = \"odd\"\n");
}

// Exercises the real wtr.watcher OS path end-to-end. Tagged [.] so it's
// excluded from the default `ctest` run (it depends on watcher delivery
// latency); run explicitly via the [watched_filesystem_project_fs] tag.
TEST_CASE("WatchedFilesystemProjectFs detects a real on-disk write",
          "[.][viewer][watched_filesystem_project_fs][slow]") {
    TempProject tp{"integration"};
    tp.writeFile("detector.toml", "name = \"odd\"\n");
    WatchedFilesystemProjectFs w{std::make_unique<FilesystemProjectFs>(tp.root), 50ms};

    const auto gen0 = w.generation();

    // Give the watcher a beat to start, then modify the tree.
    std::this_thread::sleep_for(200ms);
    tp.writeFile("detector.toml", "name = \"odd2\"\n");

    bool bumped = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        w.poll();
        if (w.generation() > gen0) {
            bumped = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(bumped);
}

// Editors (Vim, Emacs, ...) often save atomically: write the new contents to
// a dot-prefixed temp file, then rename it over the real one. wtr reports
// that as a rename whose old (hidden) name is in `path_name` and whose new
// (visible) name is in `associated`; a filter that only looks at `path_name`
// drops the event as hidden-path noise and never sees the visible file
// change. Same [.] slow-integration treatment as the write test above.
TEST_CASE("WatchedFilesystemProjectFs detects a hidden-temp-file rename over a visible file",
          "[.][viewer][watched_filesystem_project_fs][slow]") {
    TempProject tp{"rename_over_visible"};
    tp.writeFile("detector.toml", "name = \"odd\"\n");
    WatchedFilesystemProjectFs w{std::make_unique<FilesystemProjectFs>(tp.root), 50ms};

    const auto gen0 = w.generation();

    std::this_thread::sleep_for(200ms);
    tp.writeFile(".detector.toml.swp", "name = \"odd2\"\n");
    std::error_code ec;
    std::filesystem::rename(tp.root / ".detector.toml.swp", tp.root / "detector.toml", ec);
    REQUIRE_FALSE(ec);

    bool bumped = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        w.poll();
        if (w.generation() > gen0) {
            bumped = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    REQUIRE(bumped);
}
