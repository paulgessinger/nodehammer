#include "project/pack.hpp"

#include "diagnostic_codes.hpp"
#include "viewer/archive_export.hpp"
#include "viewer/filesystem_project_fs.hpp"
#include "viewer/project_manifest.hpp"
#include "viewer/zip_working_set.hpp"

#include <detail/file_io.hpp>
#include <detail/zstd_io.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer::project {
namespace {

/// The deepest directory containing both paths.
///
/// The archive walk needs *one* mount point whose keys reach the entry config,
/// its includes and the geometry. Mounting each file's own parent would give two
/// roots and no key that spans them; mounting the working directory would work
/// only when the user happened to run the command from above both.
std::filesystem::path commonAncestor(std::filesystem::path a, std::filesystem::path b) {
    a = a.lexically_normal();
    b = b.lexically_normal();
    std::filesystem::path shared;
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end() && ib != b.end() && *ia == *ib; ++ia, ++ib) {
        shared /= *ia;
    }
    return shared;
}

std::filesystem::path requireExisting(const std::filesystem::path &p, std::string_view what) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::canonical(p, ec);
    if (ec) {
        throw Error{codes::kFatalProjectPack, std::format("{} not found: {}", what, p.string()),
                    p.string()};
    }
    return abs;
}

/// A key for `file` under `root`, or nothing when `file` is not under it.
///
/// Checked rather than assumed: `lexically_relative` happily answers with
/// leading `..`, and a key like `../../odd.nhb.zst` produces a zip entry no
/// reader will resolve — the archive would look fine and open empty.
std::optional<std::string> keyUnder(const std::filesystem::path &root,
                                    const std::filesystem::path &file) {
    const auto rel = file.lexically_relative(root);
    if (rel.empty()) {
        return std::nullopt;
    }
    for (const auto &part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    return rel.generic_string();
}

bool hasExtCi(const std::filesystem::path &path, std::string_view ext) {
    auto have = path.extension().string();
    std::ranges::transform(have, have.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return have == ext;
}

/// Whether the geometry is already the blob an archive wants to carry.
///
/// `.nhb` and `.nhb.zst` are the two spellings the FlatBuffer importer answers
/// to, and both are readable from archive bytes — `importFromBytes` decides on
/// zstd from the entry name — so either can be embedded verbatim.
bool isBlob(const std::filesystem::path &path) {
    if (hasExtCi(path, ".nhb")) {
        return true;
    }
    return hasExtCi(path, ".zst") && hasExtCi(path.stem(), ".nhb");
}

} // namespace

PackResult pack(const PackOptions &options) {
    if (options.config.empty() || options.geometry.empty()) {
        throw Error{codes::kFatalProjectPack,
                    "a project needs both an entry config and the geometry it names"};
    }

    const std::filesystem::path configAbs = requireExisting(options.config, "config file");
    const std::filesystem::path geometryAbs = requireExisting(options.geometry, "input file");
    if (configAbs == geometryAbs) {
        throw Error{codes::kFatalProjectPack, "the config and the input name the same file",
                    options.config.string()};
    }

    const bool embed = isBlob(geometryAbs);

    // An imported geometry does not come out of the mount, so the mount only has
    // to reach the config and its includes. That is what lets `-i` name a file
    // anywhere -- a detector description under /cvmfs, say -- without dragging
    // the mount up to a shared ancestor and rewriting every key.
    std::filesystem::path mount;
    if (!options.root.empty()) {
        mount = requireExisting(options.root, "root directory");
    } else if (embed) {
        mount = commonAncestor(configAbs.parent_path(), geometryAbs.parent_path());
    } else {
        mount = configAbs.parent_path();
    }
    if (mount.empty()) {
        throw Error{codes::kFatalProjectPack,
                    "the config and the input share no common directory to pack from; "
                    "name one with --root"};
    }

    const auto configKey = keyUnder(mount, configAbs);
    if (!configKey) {
        throw Error{codes::kFatalProjectPack,
                    std::format("the config is not under the pack root {}", mount.string()),
                    configAbs.string()};
    }

    std::string geometryKey;
    if (embed) {
        const auto key = keyUnder(mount, geometryAbs);
        if (!key) {
            throw Error{codes::kFatalProjectPack,
                        std::format("the input is not under the pack root {}", mount.string()),
                        geometryAbs.string()};
        }
        geometryKey = *key;
    } else {
        // At the archive root, and named for the source. Not under the source's
        // own directory: the blob is something this command produced, so putting
        // it where the input happened to live would imply it came from there.
        geometryKey = geometryAbs.stem().string() + ".nhb.zst";
    }

    viewer::FilesystemProjectFs fs{mount};
    std::vector<std::string> skipped;
    // The walk is asked for the config's closure and nothing else -- no geometry
    // key, so it does not stamp the manifest either. Both are written in below.
    //
    // Withholding the geometry was already necessary for an imported input,
    // which is not in the mount for the walk to find. Doing it for an embedded
    // one too costs a read this file was going to do anyway and buys the check
    // underneath: with the stamp deferred, an entry sitting on the manifest key
    // is still visible here, rather than already overwritten by it.
    viewer::ZipWorkingSet ws = viewer::buildArchiveWorkingSet(fs, *configKey, "", &skipped);
    if (!skipped.empty()) {
        // Not fatal on its own -- the walk reports what it could not resolve and
        // an archive missing an include still opens -- but silently publishing a
        // partial scene is worse than refusing, because the failure surfaces in
        // a browser as a build error with no mention of this machine.
        throw Error{codes::kFatalProjectPack,
                    std::format("cannot pack: {} unreadable entr{}, first is '{}'", skipped.size(),
                                skipped.size() == 1 ? "y" : "ies", skipped.front()),
                    skipped.front()};
    }

    // The manifest key belongs to the archive, not to what is being packed. The
    // stamp below would overwrite whatever the walk put here and report nothing,
    // and the archive would open naming a config whose bytes are the manifest's
    // own -- so this refuses instead. Reached by a config *named*
    // `nodehammer.toml` and by an include that resolves to it, which is why the
    // question is asked of the packed key set rather than of `configKey`.
    if (ws.contains(viewer::kProjectManifestKey)) {
        throw Error{codes::kFatalProjectPack,
                    std::format("'{}' is the archive's own manifest key: rename the file that "
                                "packs to it, or pack from a root that does not reach it",
                                viewer::kProjectManifestKey),
                    std::string{viewer::kProjectManifestKey}};
    }

    if (embed) {
        // Read here rather than through the walk, so `readFile`'s
        // `std::runtime_error` -- for a file that vanished between the check
        // above and now, or that cannot be opened -- leaves as the `Error` every
        // caller of this function reports.
        try {
            ws.writeEntry(geometryKey, detail::file_io::readFile(geometryAbs));
        } catch (const Error &) {
            throw;
        } catch (const std::exception &ex) {
            throw Error{codes::kFatalProjectPack,
                        std::format("cannot read the input: {}", ex.what()), geometryAbs.string()};
        }
    } else {
        const SemanticResult imported = SemanticScene::read(geometryAbs);
        const std::vector<std::byte> nhb = imported.scene.toNhb();
        ws.writeEntry(geometryKey, detail::zstd_io::compress(nhb));
    }

    // One manifest write for both kinds of input, from the same serializer the
    // walk would have used, so the two paths cannot drift into two shapes.
    const auto toml =
        viewer::serializeProjectManifest(viewer::ProjectManifest{*configKey, geometryKey});
    const auto *first = reinterpret_cast<const std::byte *>(toml.data());
    ws.writeEntry(std::string{viewer::kProjectManifestKey},
                  std::vector<std::byte>(first, first + toml.size()));

    return PackResult{ws.serialize(), mount, *configKey, geometryKey, !embed};
}

} // namespace nodehammer::project
