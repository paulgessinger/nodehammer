#include "web/stage.hpp"

#include "diagnostic_codes.hpp"
#include "viewer/archive_export.hpp"
#include "viewer/filesystem_project_fs.hpp"
#include "viewer/zip_working_set.hpp"

#include <nodehammer/diagnostics.hpp>

#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace nodehammer::web {
namespace {

constexpr std::string_view kSidecar = "nh_manifest.json";

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

std::string keyUnder(const std::filesystem::path &root, const std::filesystem::path &file) {
    return file.lexically_relative(root).generic_string();
}

std::filesystem::path requireExisting(const std::filesystem::path &p, std::string_view what) {
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::canonical(p, ec);
    if (ec) {
        throw Error{codes::kFatalWebStage, std::format("{} not found: {}", what, p.string()),
                    p.string()};
    }
    return abs;
}

void writeBytes(const std::filesystem::path &target, std::span<const std::byte> bytes) {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw Error{codes::kFatalWebStage, "cannot write into the staged root", target.string()};
    }
}

/// Pack a loose config + geometry into an archive, includes and all.
///
/// The same `buildArchiveWorkingSet` the viewer's "Create archive from scene"
/// uses, so a CLI-staged publication and a hand-published one have the same
/// shape -- including the root `nodehammer.toml` that makes an archive
/// self-describing, which that function writes itself.
std::vector<std::byte> packLooseFiles(const std::filesystem::path &config,
                                      const std::filesystem::path &geometry) {
    const std::filesystem::path configAbs = requireExisting(config, "config file");
    const std::filesystem::path geometryAbs = requireExisting(geometry, "input file");
    if (configAbs == geometryAbs) {
        throw Error{codes::kFatalWebStage, "--config and --input name the same file",
                    config.string()};
    }

    const std::filesystem::path mount =
        commonAncestor(configAbs.parent_path(), geometryAbs.parent_path());
    if (mount.empty()) {
        throw Error{codes::kFatalWebStage,
                    "config and input share no common directory to pack from"};
    }

    viewer::FilesystemProjectFs fs{mount};
    std::vector<std::string> skipped;
    viewer::ZipWorkingSet ws = viewer::buildArchiveWorkingSet(
        fs, keyUnder(mount, configAbs), keyUnder(mount, geometryAbs), &skipped);
    if (!skipped.empty()) {
        // Not fatal on its own -- the walk reports what it could not resolve and
        // an archive missing an include still opens -- but silently publishing a
        // partial scene is worse than refusing, because the failure surfaces in
        // a browser as a build error with no mention of this machine.
        throw Error{codes::kFatalWebStage,
                    std::format("cannot pack: {} unreadable entr{}, first is '{}'", skipped.size(),
                                skipped.size() == 1 ? "y" : "ies", skipped.front()),
                    skipped.front()};
    }
    return ws.serialize();
}

} // namespace

StagedRoot stageRoot(const StageOptions &options) {
    std::error_code ec;
    if (!std::filesystem::is_directory(options.runtime, ec)) {
        throw Error{codes::kFatalWebStage, "no runtime directory to stage from",
                    options.runtime.string()};
    }
    // create_directories, not create_directory: harmless when the caller already
    // made the directory (the CLI does, with mkdtemp, so that it is 0700 rather
    // than 0755 in a shared /tmp) and correct when it did not.
    std::filesystem::create_directories(options.target, ec);
    if (ec) {
        throw Error{codes::kFatalWebStage, "cannot create the staged root",
                    options.target.string()};
    }

    // The runtime, verbatim. Copied rather than linked: the staged root outlives
    // nothing and may be served after the runtime it came from is gone (a
    // rebuild, an unmounted share), and a dangling symlink serves a 404 that
    // names the wrong problem.
    for (const auto &entry : std::filesystem::directory_iterator(options.runtime)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::filesystem::copy_file(entry.path(), options.target / entry.path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            throw Error{
                codes::kFatalWebStage,
                std::format("cannot stage {}: {}", entry.path().filename().string(), ec.message()),
                entry.path().string()};
        }
    }

    StagedRoot staged{options.target, Posture::Application, {}};

    if (!options.project.empty()) {
        const std::filesystem::path archive = requireExisting(options.project, "project archive");
        staged.archive = archive.filename().string();
        std::filesystem::copy_file(archive, options.target / staged.archive,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            throw Error{codes::kFatalWebStage,
                        std::format("cannot stage the archive: {}", ec.message()),
                        archive.string()};
        }
        staged.posture = Posture::Viewer;
    } else if (!options.config.empty() || !options.geometry.empty()) {
        if (options.config.empty() || options.geometry.empty()) {
            throw Error{codes::kFatalWebStage,
                        "--config and --input go together: an archive needs both an entry "
                        "config and the geometry it names"};
        }
        const std::vector<std::byte> bytes = packLooseFiles(options.config, options.geometry);
        staged.archive = "project.nhproj";
        writeBytes(options.target / staged.archive, bytes);
        staged.posture = Posture::Viewer;
    }

    // The sidecar is the posture. Written last, so a root that fails halfway
    // through never claims to be a publication it does not contain.
    const std::filesystem::path sidecar = options.target / kSidecar;
    if (staged.posture == Posture::Viewer) {
        std::string title = options.title;
        if (title.empty()) {
            title = std::format("nodehammer — {}", staged.archive);
        }
        // Hand-written rather than through a JSON library: three fields, and the
        // shape has to match what the shell already parses byte for byte.
        std::ofstream out(sidecar, std::ios::trunc);
        out << "{\n"
            << "  \"archive\": \"" << staged.archive << "\",\n"
            << "  \"lock\": true,\n"
            << "  \"title\": \"" << title << "\"\n"
            << "}\n";
        if (!out) {
            throw Error{codes::kFatalWebStage, "cannot write the sidecar", sidecar.string()};
        }
    } else {
        // Application mode is the *absence* of a sidecar, so a stale one from a
        // previous stage into this directory would silently change the posture.
        std::filesystem::remove(sidecar, ec);
    }

    return staged;
}

} // namespace nodehammer::web
