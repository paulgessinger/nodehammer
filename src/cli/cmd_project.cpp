#include "cli_common.hpp"
#include "run_internal.hpp"

#include "project/pack.hpp"
#include "viewer/project_manifest.hpp"
#include "viewer/zip_working_set.hpp"
#include "web/runtime_locator.hpp"
#include "web/stage.hpp"

#include <CLI/CLI.hpp>

#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

namespace project = nodehammer::project;
namespace viewer = nodehammer::viewer;
namespace web = nodehammer::web;

std::string optionText(const CLI::App &sub, const char *name, std::string fallback = {}) {
    const CLI::Option *opt = sub.get_option(name);
    return opt->count() > 0 ? opt->as<std::string>() : std::move(fallback);
}

void writeBytes(const std::filesystem::path &target, std::span<const std::byte> bytes) {
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw nodehammer::Error{nodehammer::codes::kFatalProjectPack, "cannot write the archive",
                                target.string()};
    }
}

/// Every file key in the archive, depth first.
///
/// `listAtPrefix` answers with one directory's children, because that is what a
/// file browser asks for; a listing wants the leaves, so the recursion lives
/// here rather than in the store.
std::vector<viewer::ZipDirEntry> allFiles(const viewer::ZipWorkingSet &ws) {
    std::vector<viewer::ZipDirEntry> files;
    std::deque<std::string> dirs;
    // A ZIP is not a tree until something checks. `dir//file` and `/file` are
    // both legal entry names, and a directory that normalises back to the prefix
    // it came from would have this loop hand it to itself forever -- on an
    // archive from anywhere, which `info` is precisely the command for. The
    // listing drops the empty segments those names produce; this refuses to walk
    // a prefix twice whatever the listing says, because a hang is the one
    // failure that reports nothing at all.
    std::unordered_set<std::string> visited;
    dirs.emplace_back();
    visited.insert("");
    while (!dirs.empty()) {
        const std::string dir = std::move(dirs.front());
        dirs.pop_front();
        for (auto &entry : ws.listAtPrefix(dir)) {
            if (entry.is_directory) {
                if (visited.insert(entry.key).second) {
                    dirs.push_back(entry.key);
                }
            } else {
                files.push_back(std::move(entry));
            }
        }
    }
    return files;
}

} // namespace

namespace nodehammer::cli::detail {

void registerCmdProject(CLI::App &app, const RunOptions &options) {
    // Copied into each callback below rather than reached through `options`:
    // it is a pointer to the caller's object, so copying it costs nothing and
    // it still sees a `-q` written during the parse.
    const Narrator say{options};

    auto *sub = app.add_subcommand("project", "Build and publish .nhproj project archives")
                    ->require_subcommand(1);

    // ── pack ─────────────────────────────────────────────────────────────────
    auto *packSub = sub->add_subcommand("pack", "Pack a config and its geometry into a .nhproj");
    packSub->add_option("-c,--config", "Entry config file (.toml / .lua)")->required();
    // Deliberately the same spelling and the same meaning as `convert -i`: any
    // format this build can import. A `.nhb`/`.nhb.zst` is embedded as-is;
    // anything else is imported here, so publishing a detector does not need a
    // separate command to produce the blob first.
    packSub->add_option("-i,--input", "Input geometry, in any importable format")->required();
    packSub->add_option("-o,--output", "Archive to write")->required()->type_name("FILE");
    packSub->add_option("--root", "Directory defining the archive's key space")->type_name("DIR");

    packSub->callback([packSub, say] {
        runOrReport("project pack", [&] {
            const project::PackResult packed =
                project::pack({.config = optionText(*packSub, "--config"),
                               .geometry = optionText(*packSub, "--input"),
                               .root = optionText(*packSub, "--root")});

            const std::filesystem::path out = optionText(*packSub, "--output");
            writeBytes(out, packed.bytes);

            // The root is reported because it is not visible anywhere else and
            // it decided every key in the archive: an include that resolves is
            // the difference between this file opening and opening empty.
            say("root      {}", packed.root.string());
            say("config    {}", packed.configKey);
            say("geometry  {}{}", packed.geometryKey, packed.imported ? " (imported)" : "");
            // The path alone on stdout, so `$(nodehammer project pack ...)` is
            // the archive and not a report about it. Not narration and not
            // silenced by `-q`: it is what the command was asked for.
            std::println("{}", out.string());
        });
    });

    // ── publish ──────────────────────────────────────────────────────────────
    auto *pubSub = sub->add_subcommand(
        "publish", "Write a self-contained static site: runtime, sidecar and archive");
    pubSub->add_option("path", "Project to publish: an existing .nhproj")->type_name("PATH");
    pubSub->add_option("-c,--config", "Entry config file, if no archive is given");
    pubSub->add_option("-i,--input", "Input geometry, if no archive is given");
    pubSub->add_option("-o,--output", "Directory to write the package into")
        ->required()
        ->type_name("DIR");
    pubSub->add_option("--title", "Browser-tab title");
    pubSub->add_option("--web-assets", "Directory holding the built wasm runtime");

    pubSub->callback([pubSub, &options, say] {
        runOrReport("project publish", [&] {
            web::LadderInputs inputs{};
            inputs.explicitDir = optionText(*pubSub, "--web-assets");
            inputs.embedderDir = options.webAssets;

            // Walked a second time only on the failure path. See
            // web/runtime_locator.hpp for why the detail is not on the exception.
            const auto locate = [&] {
                try {
                    return web::locateRuntime(inputs);
                } catch (const nodehammer::Error &) {
                    // Printed unconditionally: this is the diagnosis of a
                    // failure in progress, not an account of work going well,
                    // and `-q` does not hide those.
                    std::print(stderr, "{}\n", web::explainLadder(web::walkLadder(inputs)));
                    throw;
                }
            };
            const web::RuntimeLocation runtime = locate();

            const std::filesystem::path out = optionText(*pubSub, "--output");
            web::StageOptions stage{};
            stage.runtime = runtime.dir;
            stage.target = out;
            stage.title = optionText(*pubSub, "--title");
            stage.project = optionText(*pubSub, "path");
            stage.config = optionText(*pubSub, "--config");
            stage.geometry = optionText(*pubSub, "--input");
            const web::StagedRoot staged = web::stageRoot(stage);

            say("runtime   {}", runtime.version);
            say("posture   {}",
                staged.posture == web::Posture::Viewer ? "viewer (locked)" : "application (empty)");
            if (!staged.archive.empty()) {
                say("archive   {}", staged.archive);
            }
            std::println("{}", out.string());
        });
    });

    // ── info ─────────────────────────────────────────────────────────────────
    auto *infoSub = sub->add_subcommand("info", "Print an archive's manifest and contents");
    infoSub->add_option("path", "The .nhproj to read")->required()->type_name("PATH");

    infoSub->callback([infoSub] {
        runOrReport("project info", [&] {
            const std::filesystem::path path = optionText(*infoSub, "path");
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec)) {
                throw nodehammer::Error{nodehammer::codes::kFatalProjectPack, "no such archive",
                                        path.string()};
            }
            // `openFromFile` throws `std::runtime_error` for a file that is not
            // a readable ZIP, and `runOrReport` catches `Error` alone -- so
            // without this, `project info` on any regular file that is not an
            // archive terminates instead of reporting one.
            viewer::ZipWorkingSet ws = [&] {
                try {
                    return viewer::ZipWorkingSet::openFromFile(path);
                } catch (const nodehammer::Error &) {
                    throw;
                } catch (const std::exception &ex) {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalProjectPack,
                        std::format("cannot read the archive '{}': {}", path.string(), ex.what()),
                        path.string()};
                }
            }();

            // The manifest is what makes an archive self-describing, so its
            // absence is the headline rather than a missing line: an archive
            // without one opens blank and waits to be told what to build.
            if (const auto toml = ws.read(viewer::kProjectManifestKey)) {
                if (const auto manifest = viewer::parseProjectManifest(toml->span())) {
                    std::println("config    {}", manifest->config_key);
                    std::println("geometry  {}", manifest->geometry_key);
                } else {
                    std::println("manifest  present but names no entry keys");
                }
            } else {
                std::println("manifest  absent — this archive opens blank");
            }

            const auto files = allFiles(ws);
            std::uint64_t total = 0;
            for (const auto &f : files) {
                total += f.bytes;
            }
            std::println("entries   {} ({} bytes uncompressed)", files.size(), total);
            for (const auto &f : files) {
                std::println("  {:>12}  {}", f.bytes, f.key);
            }
        });
    });
}

} // namespace nodehammer::cli::detail
