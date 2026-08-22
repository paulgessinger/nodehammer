#include "web/stage.hpp"

#include "diagnostic_codes.hpp"
#include "project/pack.hpp"

#include <nodehammer/diagnostics.hpp>

#include <format>
#include <fstream>
#include <memory>
#include <vector>

namespace nodehammer::web {
namespace {

constexpr std::string_view kSidecar = "nh_manifest.json";

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
        // The same packer `nodehammer project pack` uses, so a root staged on
        // the way to a browser and an archive written to a path are the same
        // bytes rather than two implementations that agree today.
        const project::PackResult packed =
            project::pack({.config = options.config, .geometry = options.geometry});
        staged.archive = "project.nhproj";
        writeBytes(options.target / staged.archive, packed.bytes);
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
