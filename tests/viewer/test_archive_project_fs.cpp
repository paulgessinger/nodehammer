#include <nodehammer/viewer/archive_project_fs.hpp>

#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/ir/fb/semantic/flatbuffer.hpp>
#include <nodehammer/ir/semantic.hpp>
#include <nodehammer/viewer/build_session.hpp>
#include <nodehammer/viewer/project_fs.hpp>
#include <nodehammer/viewer/zip_working_set.hpp>

#include <catch2/catch_test_macros.hpp>

// miniz's zlib-compat aliases (`compress`, `uncompress`, …) would macro-clash
// with nodehammer::zstd_io::compress used by the build-session harness below.
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using nodehammer::viewer::ArchiveProjectFs;
using nodehammer::viewer::DirNode;
using nodehammer::viewer::ProjectDropDecision;
using nodehammer::viewer::ProjectFsStatus;
using nodehammer::viewer::ResolveStatus;
using nodehammer::viewer::ZipWorkingSet;

namespace {

std::string asString(std::span<const std::byte> sp) {
    return std::string{reinterpret_cast<const char *>(sp.data()), sp.size()};
}

std::vector<std::byte> asBytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

std::vector<std::byte> makeZip(const std::vector<std::pair<std::string, std::string>> &entries) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto &[name, contents] : entries) {
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), contents.data(), contents.size(),
                                      static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)));
    }
    void *ptr = nullptr;
    std::size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &ptr, &size));
    std::vector<std::byte> out(size);
    std::memcpy(out.data(), ptr, size);
    mz_zip_writer_end(&zip);
    return out;
}

void writeLe32(std::vector<std::byte> &bytes, std::size_t off, std::uint32_t value) {
    bytes[off] = static_cast<std::byte>(value & 0xffU);
    bytes[off + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[off + 2] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[off + 3] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

void corruptZipCrc(std::vector<std::byte> &bytes) {
    constexpr std::array<std::byte, 4> kLocalSig{std::byte{0x50}, std::byte{0x4b}, std::byte{0x03},
                                                 std::byte{0x04}};
    constexpr std::array<std::byte, 4> kCentralSig{std::byte{0x50}, std::byte{0x4b},
                                                   std::byte{0x01}, std::byte{0x02}};
    auto local = std::search(bytes.begin(), bytes.end(), kLocalSig.begin(), kLocalSig.end());
    REQUIRE(local != bytes.end());
    const std::size_t local_off = static_cast<std::size_t>(std::distance(bytes.begin(), local));
    writeLe32(bytes, local_off + 14, 0x12345678U);

    auto central = std::search(bytes.begin(), bytes.end(), kCentralSig.begin(), kCentralSig.end());
    REQUIRE(central != bytes.end());
    const std::size_t central_off = static_cast<std::size_t>(std::distance(bytes.begin(), central));
    writeLe32(bytes, central_off + 16, 0x12345678U);
}

const DirNode *findChild(std::span<const DirNode> children, std::string_view name) {
    auto it = std::ranges::find_if(children, [&](const DirNode &n) { return n.name == name; });
    return it == children.end() ? nullptr : &*it;
}

/// A minimal valid `.nhb.zst` geometry payload for the build-session harness.
std::vector<std::byte> minimalNhbZstBytes() {
    using namespace nodehammer;
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

    auto raw = semanticSceneToBytes(scene);
    return zstd_io::compress(std::span<const std::byte>{raw});
}

/// RAII temp dir holding a seed archive on disk.
struct TempArchive {
    std::filesystem::path root;
    std::filesystem::path zip;
    explicit TempArchive(std::string_view tag,
                         const std::vector<std::pair<std::string, std::string>> &entries) {
        root = std::filesystem::temp_directory_path() /
               (std::string{"nh_archive_project_fs_"} + std::string{tag});
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        zip = root / "project.zip";
        auto blob = makeZip(entries);
        nodehammer::file_io::writeFile(zip, blob);
    }
    ~TempArchive() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    TempArchive(const TempArchive &) = delete;
    TempArchive &operator=(const TempArchive &) = delete;
};

} // namespace

TEST_CASE("ArchiveProjectFs opens a zip and resolves entries", "[viewer][archive_project_fs]") {
    TempArchive ta{"resolve", {{"scene.toml", "root = 1\n"}, {"det/inner.toml", "inner = 2\n"}}};
    ArchiveProjectFs fs{ta.zip};

    REQUIRE(fs.status() == ProjectFsStatus::Ready);
    REQUIRE(fs.name() == "archive");

    auto r = fs.resolve("scene.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    REQUIRE(asString(r.file.bytes.span()) == "root = 1\n");
    REQUIRE(asString(fs.resolve("det/inner.toml").file.bytes.span()) == "inner = 2\n");

    REQUIRE(fs.resolve("nope.toml").status == ResolveStatus::Missing);
    REQUIRE(fs.resolve("../escape").status == ResolveStatus::Missing);
}

TEST_CASE("ArchiveProjectFs synthesizes a directory tree", "[viewer][archive_project_fs]") {
    TempArchive ta{"list", {{"top.toml", "t\n"}, {"det/a.toml", "a\n"}, {"det/b.toml", "b\n"}}};
    ArchiveProjectFs fs{ta.zip};

    auto root = fs.list("");
    REQUIRE(findChild(root, "top.toml") != nullptr);
    const auto *det = findChild(root, "det");
    REQUIRE(det != nullptr);
    REQUIRE(det->is_directory);

    const auto gen_before = fs.generation();
    auto under = fs.list(det->key);
    REQUIRE(findChild(under, "a.toml") != nullptr);
    REQUIRE(findChild(under, "b.toml") != nullptr);
    // Reads don't bump the generation.
    REQUIRE(fs.generation() == gen_before);
}

TEST_CASE("ArchiveProjectFs accepts drops as working-set overrides",
          "[viewer][archive_project_fs]") {
    TempArchive ta{"drop", {{"scene.toml", "old\n"}}};
    ArchiveProjectFs fs{ta.zip};

    using enum ProjectDropDecision::Kind;
    REQUIRE(fs.planAddBytes("new.toml", {}).kind == Accept);
    REQUIRE(fs.planAddBytes("scene.toml", {}).kind == Confirm);
    REQUIRE_FALSE(fs.dirty());

    const auto gen_before = fs.generation();
    auto bytes = asBytes("added\n");
    fs.addBytes("added.toml", bytes);
    REQUIRE(fs.dirty());
    REQUIRE(fs.generation() > gen_before);
    REQUIRE(asString(fs.resolve("added.toml").file.bytes.span()) == "added\n");
}

TEST_CASE("ArchiveProjectFs removes entries with a confirming plan",
          "[viewer][archive_project_fs]") {
    TempArchive ta{"remove", {{"scene.toml", "keep\n"}, {"drop.toml", "bye\n"}}};
    ArchiveProjectFs fs{ta.zip};

    using enum ProjectDropDecision::Kind;
    // A present key confirms; a missing key rejects.
    REQUIRE(fs.planRemove("drop.toml").kind == Confirm);
    REQUIRE(fs.planRemove("nope.toml").kind == Reject);

    const auto gen_before = fs.generation();
    fs.removeKey("drop.toml");
    REQUIRE(fs.resolve("drop.toml").status == ResolveStatus::Missing);
    REQUIRE(fs.resolve("scene.toml").status == ResolveStatus::Ready);
    REQUIRE(fs.generation() > gen_before);
    REQUIRE(fs.dirty());

    // Removing a now-absent key is a no-op that neither throws nor re-bumps.
    const auto gen_after = fs.generation();
    fs.removeKey("drop.toml");
    REQUIRE(fs.generation() == gen_after);
}

TEST_CASE("ArchiveProjectFs moves an entry to a new key", "[viewer][archive_project_fs]") {
    TempArchive ta{"move", {{"scene.toml", "body\n"}, {"detectors/keep.toml", "k\n"}}};
    ArchiveProjectFs fs{ta.zip};

    using enum ProjectDropDecision::Kind;
    // Free destination accepts; a landing collision confirms; missing source and
    // a same-key move reject.
    REQUIRE(fs.planMove("scene.toml", "detectors/scene.toml").kind == Accept);
    REQUIRE(fs.planMove("scene.toml", "detectors/keep.toml").kind == Confirm);
    REQUIRE(fs.planMove("nope.toml", "detectors/nope.toml").kind == Reject);
    REQUIRE(fs.planMove("scene.toml", "scene.toml").kind == Reject);

    const auto gen_before = fs.generation();
    fs.moveKey("scene.toml", "detectors/scene.toml");
    CHECK(fs.resolve("scene.toml").status == ResolveStatus::Missing);
    REQUIRE(fs.resolve("detectors/scene.toml").status == ResolveStatus::Ready);
    CHECK(asString(fs.resolve("detectors/scene.toml").file.bytes.span()) == "body\n");
    CHECK(fs.generation() > gen_before);
    CHECK(fs.dirty());
}

TEST_CASE("ArchiveProjectFs saves edits back to disk atomically", "[viewer][archive_project_fs]") {
    TempArchive ta{"save", {{"scene.toml", "v1\n"}}};
    {
        ArchiveProjectFs fs{ta.zip};
        fs.addBytes("scene.toml", asBytes("v2\n")); // replace existing
        fs.addBytes("extra.toml", asBytes("x\n"));
        REQUIRE(fs.dirty());
        REQUIRE(fs.save());
        REQUIRE_FALSE(fs.dirty()); // reopened from the saved bytes
    }

    // A fresh backend on the same path sees the persisted edits.
    ArchiveProjectFs reopened{ta.zip};
    REQUIRE(reopened.status() == ProjectFsStatus::Ready);
    REQUIRE(asString(reopened.resolve("scene.toml").file.bytes.span()) == "v2\n");
    REQUIRE(asString(reopened.resolve("extra.toml").file.bytes.span()) == "x\n");
}

TEST_CASE("ArchiveProjectFs drives a headless scene build via BuildSession",
          "[viewer][archive_project_fs]") {
    // End-to-end without the viewer: an archive holding a config + geometry pair
    // feeds the same BuildSession the App drives, proving open-archive → scene
    // build works through the ProjectFs contract alone.
    auto geom = minimalNhbZstBytes();
    std::string geom_str{reinterpret_cast<const char *>(geom.data()), geom.size()};
    TempArchive ta{"build",
                   {{"scene.toml", "# minimal nodehammer config\n"}, {"scene.nhb.zst", geom_str}}};

    ArchiveProjectFs fs{ta.zip};
    REQUIRE(fs.status() == ProjectFsStatus::Ready);

    nodehammer::viewer::BuildSession session;
    session.setRootKeys("scene.toml", "scene.nhb.zst");

    using nodehammer::viewer::BuildPhase;
    for (int i = 0; i < 100; ++i) {
        session.poll(&fs);
        const auto p = session.phase();
        if (p == BuildPhase::ResolvedReady || p == BuildPhase::Error ||
            p == BuildPhase::WaitingForUser) {
            break;
        }
    }

    REQUIRE(session.phase() == BuildPhase::ResolvedReady);
    auto inputs = session.takeInputs();
    REQUIRE(inputs);
    REQUIRE_FALSE(inputs->import.diags.hasErrors());
    REQUIRE(inputs->import.scene.nodes.contains(inputs->import.scene.rootId));
}

TEST_CASE("ArchiveProjectFs enters an error state on a bad archive",
          "[viewer][archive_project_fs]") {
    auto root = std::filesystem::temp_directory_path() / "nh_archive_project_fs_bad";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    auto bad = root / "not.zip";
    nodehammer::file_io::writeFile(bad, asBytes("definitely not a zip"));

    ArchiveProjectFs fs{bad};
    REQUIRE(fs.status() == ProjectFsStatus::Error);
    REQUIRE(fs.resolve("anything").status == ResolveStatus::Error);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST_CASE("ArchiveProjectFs surfaces corrupted archive entries as errors",
          "[viewer][archive_project_fs]") {
    TempArchive ta{"corrupt", {{"scene.toml", "root = 1\n"}}};

    auto bytes = nodehammer::file_io::readFile(ta.zip);
    corruptZipCrc(bytes);

    const auto corrupt_zip = ta.root / "corrupt.zip";
    nodehammer::file_io::writeFile(corrupt_zip, bytes);

    ArchiveProjectFs fs{corrupt_zip};
    REQUIRE(fs.status() == ProjectFsStatus::Ready);
    REQUIRE(fs.resolve("scene.toml").status == ResolveStatus::Error);
}

TEST_CASE("ArchiveProjectFs Empty provenance falls back to basename for flat loose drops",
          "[viewer][archive_project_fs]") {
    // A scratch working set assembled from flat loose drops (the web empty
    // project): an include referencing a subdir resolves to the root-dropped
    // basename so the graph still links.
    ArchiveProjectFs scratch{ZipWorkingSet::create(), ArchiveProjectFs::Provenance::Empty};
    REQUIRE(scratch.provenance() == ArchiveProjectFs::Provenance::Empty);
    scratch.addBytes("scene.toml", asBytes("include = [\"materials/common.toml\"]\n"));
    scratch.addBytes("common.toml", asBytes("shared = 1\n"));

    auto r = scratch.resolve("materials/common.toml");
    REQUIRE(r.status == ResolveStatus::Ready);
    REQUIRE(asString(r.file.bytes.span()) == "shared = 1\n");

    // A real archive (Local/Remote provenance) keeps strict full-path resolution:
    // no basename fallback, because its keys are already full paths.
    ArchiveProjectFs opened{
        ZipWorkingSet::openFromBytes(makeZip({{"common.toml", "shared = 1\n"}})),
        ArchiveProjectFs::Provenance::Local};
    REQUIRE(opened.provenance() == ArchiveProjectFs::Provenance::Local);
    REQUIRE(opened.resolve("materials/common.toml").status == ResolveStatus::Missing);
    REQUIRE(opened.resolve("common.toml").status == ResolveStatus::Ready);
}
