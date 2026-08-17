#include <viewer/archive_export.hpp>

#include <detail/file_io.hpp>
#include <viewer/archive_project_fs.hpp>
#include <viewer/filesystem_project_fs.hpp>
#include <viewer/project_manifest.hpp>
#include <viewer/zip_working_set.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using nodehammer::viewer::ArchiveProjectFs;
using nodehammer::viewer::buildArchiveWorkingSet;
using nodehammer::viewer::FilesystemProjectFs;
using nodehammer::viewer::parseProjectManifest;
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

/// RAII temp directory holding loose project files on disk.
struct TempDir {
    std::filesystem::path root;
    explicit TempDir(std::string_view tag) {
        root = std::filesystem::temp_directory_path() /
               (std::string{"nh_archive_export_"} + std::string{tag});
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    void write(const std::string &rel, std::string_view contents) const {
        const auto p = root / rel;
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out{p, std::ios::binary | std::ios::trunc};
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }
};

} // namespace

TEST_CASE("buildArchiveWorkingSet captures only the build closure for filesystem mode",
          "[viewer][archive_export]") {
    TempDir dir{"closure"};
    // scene.toml includes common.toml but NOT other.toml; other.toml sits in the
    // same folder and must be excluded from the closure.
    dir.write("scene.toml", "include = [\"common.toml\"]\n");
    dir.write("common.toml", "shared = 1\n");
    dir.write("other.toml", "unrelated = 2\n");
    dir.write("scene.nhb.zst", "GEOMETRY-BLOB");

    FilesystemProjectFs fs{dir.root};
    REQUIRE_FALSE(fs.listingIsComplete()); // filesystem → closure path

    auto ws = buildArchiveWorkingSet(fs, "scene.toml", "scene.nhb.zst");
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());

    REQUIRE(asString(out.read("scene.toml")->span()) == "include = [\"common.toml\"]\n");
    REQUIRE(asString(out.read("common.toml")->span()) == "shared = 1\n");
    REQUIRE(asString(out.read("scene.nhb.zst")->span()) == "GEOMETRY-BLOB");
    // The unreferenced sibling is NOT swept in.
    REQUIRE_FALSE(out.contains("other.toml"));
}

TEST_CASE("buildArchiveWorkingSet captures the whole working set for complete backends",
          "[viewer][archive_export]") {
    // An unbound archive is exhaustively listable, so every entry — including a
    // config the current build doesn't reference — is exported.
    auto seed = ZipWorkingSet::create();
    seed.writeEntry("scene.toml", asBytes("include = [\"common.toml\"]\n"));
    seed.writeEntry("common.toml", asBytes("shared = 1\n"));
    seed.writeEntry("alt/experiment.toml", asBytes("alt = 3\n")); // unreferenced extra
    seed.writeEntry("scene.nhb.zst", asBytes("GEOMETRY-BLOB"));

    ArchiveProjectFs fs{std::move(seed)};
    REQUIRE(fs.listingIsComplete());

    auto ws = buildArchiveWorkingSet(fs, "scene.toml", "scene.nhb.zst");
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());

    REQUIRE(out.contains("scene.toml"));
    REQUIRE(out.contains("common.toml"));
    REQUIRE(out.contains("scene.nhb.zst"));
    REQUIRE(asString(out.read("alt/experiment.toml")->span()) == "alt = 3\n");
}

TEST_CASE("ArchiveProjectFs unbound bundle saves and binds to a path", "[viewer][archive_export]") {
    auto seed = ZipWorkingSet::create();
    seed.writeEntry("scene.toml", asBytes("root = 1\n"));
    seed.writeEntry("det/inner.toml", asBytes("inner = 2\n"));

    ArchiveProjectFs fs{std::move(seed)};
    REQUIRE_FALSE(fs.isBound());
    REQUIRE(fs.dirty()); // fresh in-memory content is unsaved
    REQUIRE(fs.path().empty());
    REQUIRE(asString(fs.resolve("scene.toml").file.bytes.span()) == "root = 1\n");

    // serialize() yields a reopenable ZIP of the current working set.
    auto blob = fs.serialize();
    REQUIRE_FALSE(blob.empty());
    auto peek = ZipWorkingSet::openFromBytes(blob);
    REQUIRE(asString(peek.read("det/inner.toml")->span()) == "inner = 2\n");

    // save() fails while unbound; saveTo binds and clears dirty.
    REQUIRE_FALSE(fs.save());

    TempDir dir{"unbound_save"};
    const auto target = dir.root / "bundle.zip";
    REQUIRE(fs.saveTo(target));
    REQUIRE(fs.isBound());
    REQUIRE(fs.path() == target);
    REQUIRE_FALSE(fs.dirty());

    // A fresh backend on the written path reads the same content.
    ArchiveProjectFs reopened{target};
    REQUIRE(asString(reopened.resolve("scene.toml").file.bytes.span()) == "root = 1\n");
    REQUIRE(asString(reopened.resolve("det/inner.toml").file.bytes.span()) == "inner = 2\n");
}

TEST_CASE("ArchiveProjectFs save does not bump generation", "[viewer][archive_export]") {
    // Saving persists the working set verbatim, so it changes neither the
    // resolvable content nor the listing. It must NOT bump generation() — a bump
    // would make the BuildSession re-walk and re-tessellate an identical scene.
    TempDir dir{"save_gen"};
    const auto target = dir.root / "bundle.zip";
    {
        auto seed = ZipWorkingSet::create();
        seed.writeEntry("scene.toml", asBytes("root = 1\n"));
        ArchiveProjectFs fresh{std::move(seed)};
        REQUIRE(fresh.saveTo(target));
    }

    ArchiveProjectFs fs{target};
    const auto gen_open = fs.generation();

    fs.addBytes("extra.toml", asBytes("x\n")); // an edit bumps generation
    const auto gen_edit = fs.generation();
    REQUIRE(gen_edit > gen_open);
    REQUIRE(fs.dirty());

    REQUIRE(fs.save()); // in-place save
    REQUIRE_FALSE(fs.dirty());
    REQUIRE(fs.generation() == gen_edit); // the save itself did not bump
}

TEST_CASE("buildArchiveWorkingSet captures a Lua config's computed closure",
          "[viewer][archive_export]") {
    // The closure of a script cannot be read off its source: `include()` is a
    // call, and here the argument is built at run time. Running it against a
    // fetcher that packs what it serves is what makes the archive complete —
    // and the unreferenced sibling still has to stay out, so this is discovery
    // rather than a sweep.
    TempDir dir{"lua_closure"};
    dir.write("scene.lua", R"LUA(
local part = "materials"
include(part .. ".lua")
)LUA");
    dir.write("materials.lua", "local P = use(\"lib/palette.lua\")\nmaterial(\"m\", {})\n");
    dir.write("lib/palette.lua", "return { silicon = \"#808080\" }\n");
    dir.write("unrelated.lua", "material(\"nope\", {})\n");
    dir.write("scene.nhb.zst", "GEOMETRY-BLOB");

    FilesystemProjectFs fs{dir.root};
    REQUIRE_FALSE(fs.listingIsComplete()); // filesystem → closure path

    auto ws = buildArchiveWorkingSet(fs, "scene.lua", "scene.nhb.zst");
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());

    REQUIRE(out.contains("scene.lua"));
    REQUIRE(out.contains("materials.lua"));   // reached by a computed include()
    REQUIRE(out.contains("lib/palette.lua")); // reached by use() one level deeper
    REQUIRE(out.contains("scene.nhb.zst"));
    REQUIRE_FALSE(out.contains("unrelated.lua"));
}

// Packing every byte is only half of it: something has to name the entry points,
// or the archive opens blank. This is the failure the content assertions above
// cannot see — they pass on an archive that is complete and still unbuildable.
TEST_CASE("buildArchiveWorkingSet stamps the roots into a manifest",
          "[viewer][archive_export]") {
    TempDir dir{"manifest"};
    dir.write("scene.lua", "material(\"m\", {})\n");
    dir.write("scene.nhb.zst", "GEOMETRY-BLOB");

    FilesystemProjectFs fs{dir.root};
    auto ws = buildArchiveWorkingSet(fs, "scene.lua", "scene.nhb.zst");
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());

    const auto manifest = out.read("nodehammer.toml");
    REQUIRE(manifest.has_value());

    // Round-trips through the parser the viewer actually opens archives with.
    const auto parsed = parseProjectManifest(manifest->span());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->config_key == "scene.lua");
    REQUIRE(parsed->geometry_key == "scene.nhb.zst");
}

// A complete backend copies whatever it is holding, manifest included. The
// export still has to describe the roots being exported now, not the ones some
// earlier export happened to record.
TEST_CASE("buildArchiveWorkingSet refreshes a stale manifest on a complete backend",
          "[viewer][archive_export]") {
    auto seed = ZipWorkingSet::create();
    seed.writeEntry("nodehammer.toml",
                    asBytes("[project]\nconfig = 'old.toml'\ngeometry = 'old.nhb.zst'\n"));
    seed.writeEntry("scene.toml", asBytes("root = 1\n"));
    seed.writeEntry("scene.nhb.zst", asBytes("GEOMETRY-BLOB"));

    ArchiveProjectFs fs{std::move(seed)};
    REQUIRE(fs.listingIsComplete());

    auto ws = buildArchiveWorkingSet(fs, "scene.toml", "scene.nhb.zst");
    auto out = ZipWorkingSet::openFromBytes(ws.serialize());

    const auto parsed = parseProjectManifest(out.read("nodehammer.toml")->span());
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->config_key == "scene.toml");
    REQUIRE(parsed->geometry_key == "scene.nhb.zst");
}
