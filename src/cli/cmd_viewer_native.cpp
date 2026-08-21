#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <detail/file_io.hpp>
#include <viewer/app.hpp>
#include <viewer/archive_project_fs.hpp>
#include <viewer/bag_project_fs.hpp>
#include <viewer/config.hpp>
#include <viewer/filesystem_project_fs.hpp>
#include <viewer/watched_filesystem_project_fs.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>

namespace {

std::optional<std::string> relativeKeyUnder(std::filesystem::path root,
                                            std::filesystem::path file) {
    root = root.lexically_normal();
    file = file.lexically_normal();

    auto rel = file.lexically_relative(root);
    if (rel.empty() || rel.is_absolute()) {
        return std::nullopt;
    }
    for (const auto &part : rel) {
        if (part == "..") {
            return std::nullopt;
        }
    }
    return rel.generic_string();
}

/// Run the window, and let its exit code be the command's.
///
/// It used to be printed and then dropped on the floor: `main` returned 0 no
/// matter what the viewer reported. Three call sites said so identically, which
/// is why the fix is one function rather than three edits.
void runViewer(nodehammer::viewer::App::Handle &application) {
    const int rc = application->run();
    if (rc != 0) {
        std::println(stderr, "viewer exited with code {}", rc);
        throw nodehammer::cli::detail::CommandFailure{rc};
    }
}

bool isZipPath(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".zip";
}

} // namespace

namespace nodehammer::cli::detail {

void registerCmdViewerNative(CLI::App &app, const RunOptions &options) {
    // Extends, does not create. `registerCmdViewer` in the library owns the
    // subcommand and its shared options, because `viewer --web` has to exist in
    // builds this file is not compiled into -- a wheel above all. What is left
    // here is the window: its options, and the run path plain `viewer` takes.
    CLI::App *sub = app.get_subcommand("viewer");

    auto cfg = std::make_shared<nodehammer::viewer::Config>();
    auto initialCamera = std::make_shared<nodehammer::viewer::Camera>();
    auto cameraYawDeg = std::make_shared<float>(0.f);
    auto cameraPitchDeg = std::make_shared<float>(0.f);
    sub->add_option("--width", cfg->width, "Initial window width in pixels")->capture_default_str();
    sub->add_option("--height", cfg->height, "Initial window height in pixels")
        ->capture_default_str();
    sub->add_flag("!--no-vsync", cfg->vsync, "Disable vsync (default: vsync on)");
    auto cullModeStr = std::make_shared<std::string>("auto");
    auto *cullModeOpt =
        sub->add_option("--cull", *cullModeStr,
                        "Backface cull override: auto (per material), force-on, force-off")
            ->capture_default_str();
    auto *pauseWhenUnfocusedOpt =
        sub->add_flag("!--no-pause-when-unfocused", cfg->pause_when_unfocused,
                      "Keep rendering when the viewer is unfocused")
            ->capture_default_str();
    auto *autoOrbitOpt =
        sub->add_flag("--auto-orbit", cfg->auto_orbit, "Start with camera auto-orbit enabled")
            ->capture_default_str();
    auto *orbitSpeedOpt =
        sub->add_option("--orbit-speed", cfg->auto_orbit_speed_deg, "Auto-orbit speed in degrees/s")
            ->capture_default_str();
    auto *angleCutOpt = sub->add_flag("--angle-cut", cfg->angle_cut, "Start with angle cut enabled")
                            ->capture_default_str();
    auto *shaderAngleCutOpt = sub->add_flag("!--no-shader-angle-cut", cfg->shader_angle_cut,
                                            "Disable shader-side angle cut")
                                  ->capture_default_str();
    auto *cutStartOpt =
        sub->add_option("--cut-start", cfg->angle_cut_start_deg, "Angle cut start in degrees")
            ->capture_default_str();
    auto *cutEndOpt =
        sub->add_option("--cut-end", cfg->angle_cut_end_deg, "Angle cut end in degrees")
            ->capture_default_str();
    auto *pbrOpt = sub->add_flag("!--no-pbr", cfg->enable_pbr, "Disable PBR/IBL shading")
                       ->capture_default_str();
    auto *cameraTargetXOpt = sub->add_option("--camera-target-x", initialCamera->target.x,
                                             "Initial camera target X coordinate");
    auto *cameraTargetYOpt = sub->add_option("--camera-target-y", initialCamera->target.y,
                                             "Initial camera target Y coordinate");
    auto *cameraTargetZOpt = sub->add_option("--camera-target-z", initialCamera->target.z,
                                             "Initial camera target Z coordinate");
    auto *cameraDistanceOpt = sub->add_option("--camera-distance", initialCamera->distance,
                                              "Initial camera orbit distance");
    auto *cameraYawOpt =
        sub->add_option("--camera-yaw", *cameraYawDeg, "Initial camera yaw in degrees");
    auto *cameraPitchOpt =
        sub->add_option("--camera-pitch", *cameraPitchDeg, "Initial camera pitch in degrees");

    // -i/--input, -c/--config, `path` and --title belong to the library's half
    // of this command and are read off the subcommand below. A .nhproj path opens
    // as an ArchiveProjectFs; a directory
    // opens as a live FilesystemProjectFs. When `path` is set, --config /
    // --input (if given) name the root keys *inside* the project; otherwise it
    // opens and the user picks roots from the project panel.

    // Headless screenshot mode: render one high-res PNG (all quality maxed) once
    // the scene settles, then quit. Useful for CI thumbnails / automated renders.
    auto screenshot = std::make_shared<nodehammer::viewer::PngExportSettings>();
    auto *screenshotOpt =
        sub->add_option("--screenshot", "Render a PNG to this path on startup, then quit");
    sub->add_option("--screenshot-width", screenshot->out_width, "Screenshot output width")
        ->capture_default_str();
    sub->add_option("--screenshot-height", screenshot->out_height, "Screenshot output height")
        ->capture_default_str();
    sub->add_option("--screenshot-supersample", screenshot->supersample,
                    "Screenshot supersampling factor (1-4)")
        ->capture_default_str();

    // Headless benchmark mode: drive a fixed camera/state sequence (cut-on
    // hold/orbit/zoom, then cut-off), measure per-pass GPU time over each window,
    // grab a screenshot per segment, write the results JSON to this path (and
    // stdout), then quit. D3D11 only for the GPU timings.
    auto *benchOpt =
        sub->add_option("--bench", "Run the headless GPU benchmark, write results JSON here, quit");
    auto benchScale = std::make_shared<float>(1.0f);
    sub->add_option("--bench-scale", *benchScale,
                    "Bench SSAA on the scene/AO passes (render_scale, 0.25-4). >1 pushes the GPU "
                    "past the refresh so total/composite timings aren't present-paced")
        ->capture_default_str();

    // Replaces the library's callback rather than filling a slot it left: with a
    // window in the build, *this* file is the one that has to choose between the
    // two modes, and one dispatch per binary beats a callback pointer handed
    // across a library boundary.
    sub->callback([sub, &options, cfg, initialCamera, cameraYawDeg, cameraPitchDeg, cullModeOpt,
                   cullModeStr, screenshot, screenshotOpt, pauseWhenUnfocusedOpt, autoOrbitOpt,
                   orbitSpeedOpt, angleCutOpt, shaderAngleCutOpt, cutStartOpt, cutEndOpt, pbrOpt,
                   cameraTargetXOpt, cameraTargetYOpt, cameraTargetZOpt, cameraDistanceOpt,
                   cameraYawOpt, cameraPitchOpt, benchOpt, benchScale]() {
        if (viewerWebRequested(*sub)) {
            runViewerWeb(*sub, options);
            return;
        }

        // The only command that never had one. Every other `cmd_*.cpp` has run
        // its body through this since the error model landed; this one instead
        // validated by hand and called `std::exit`, which meant a
        // `file_io::readFile` throw below had no handler at all and reached
        // `std::terminate`.
        runOrReport("viewer", [&] {
            if (*cullModeOpt) {
                using nodehammer::viewer::CullOverride;
                CullOverride mode = CullOverride::Auto;
                if (*cullModeStr == "force-on") {
                    mode = CullOverride::ForceCull;
                } else if (*cullModeStr == "force-off") {
                    mode = CullOverride::ForceNoCull;
                } else if (*cullModeStr != "auto") {
                    throw CLI::ValidationError("--cull",
                                               "must be one of: auto, force-on, force-off");
                }
                cfg->cull = mode;
                cfg->startup_overrides.cull = mode;
            }
            if (*pauseWhenUnfocusedOpt) {
                cfg->startup_overrides.pause_when_unfocused = cfg->pause_when_unfocused;
            }
            if (*autoOrbitOpt) {
                cfg->startup_overrides.auto_orbit = cfg->auto_orbit;
            }
            if (*orbitSpeedOpt) {
                cfg->startup_overrides.auto_orbit_speed_deg = cfg->auto_orbit_speed_deg;
            }
            if (*angleCutOpt) {
                cfg->startup_overrides.angle_cut = cfg->angle_cut;
            }
            if (*shaderAngleCutOpt) {
                cfg->startup_overrides.shader_angle_cut = cfg->shader_angle_cut;
            }
            if (*cutStartOpt) {
                cfg->startup_overrides.angle_cut_start_deg = cfg->angle_cut_start_deg;
            }
            if (*cutEndOpt) {
                cfg->startup_overrides.angle_cut_end_deg = cfg->angle_cut_end_deg;
            }
            if (*pbrOpt) {
                cfg->startup_overrides.enable_pbr = cfg->enable_pbr;
            }

            const bool hasCameraOption = *cameraTargetXOpt || *cameraTargetYOpt ||
                                         *cameraTargetZOpt || *cameraDistanceOpt || *cameraYawOpt ||
                                         *cameraPitchOpt;
            const bool hasAllCameraOptions = *cameraTargetXOpt && *cameraTargetYOpt &&
                                             *cameraTargetZOpt && *cameraDistanceOpt &&
                                             *cameraYawOpt && *cameraPitchOpt;
            if (hasCameraOption && !hasAllCameraOptions) {
                throw nodehammer::Error{
                    nodehammer::codes::kFatalCliUsage,
                    "camera URL restore requires target x/y/z, distance, yaw, and pitch"};
            }
            if (hasAllCameraOptions) {
                if (initialCamera->distance <= 0.f) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliUsage,
                                            "--camera-distance must be positive"};
                }
                initialCamera->yaw = glm::radians(*cameraYawDeg);
                initialCamera->pitch = glm::radians(*cameraPitchDeg);
                cfg->initial_camera = *initialCamera;
                cfg->startup_overrides.camera = *initialCamera;
            }

            // Read off the subcommand rather than from Options this file owns:
            // the library declared them, and the parser is where the values are.
            const auto text = [sub](const char *name) {
                const CLI::Option *opt = sub->get_option(name);
                return opt->count() > 0 ? opt->as<std::string>() : std::string{};
            };
            const std::string inputPath = text("--input");
            const std::string configPath = text("--config");
            std::string projectPath = text("path");
            if (const std::string title = text("--title"); !title.empty()) {
                cfg->title = title;
            }

            std::string screenshotPath;
            if (*screenshotOpt) {
                screenshotOpt->results(screenshotPath);
                if (inputPath.empty()) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliUsage,
                                            "--screenshot requires --input (no scene to render)"};
                }
            }

            std::string benchPath;
            if (*benchOpt) {
                benchOpt->results(benchPath);
                if (inputPath.empty()) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliUsage,
                                            "--bench requires --input (no scene to render)"};
                }
            }

            // With no positional path and no --input, open the current working
            // directory as a live filesystem project. Routes through the directory
            // branch below.
            if (projectPath.empty() && inputPath.empty()) {
                std::error_code cwd_ec;
                const auto cwd = std::filesystem::current_path(cwd_ec);
                if (cwd_ec) {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalCliPathNotFound,
                        std::format("cannot read current working directory: {}", cwd_ec.message())};
                }
                projectPath = cwd.string();
            }

            nodehammer::viewer::App::Handle application(*cfg);
            if (!screenshotPath.empty()) {
                application->requestScreenshot(screenshotPath, *screenshot);
            }
            if (!benchPath.empty()) {
                application->requestBench(benchPath, inputPath, *benchScale);
            }

            // Positional project mode. A directory opens as a live
            // FilesystemProjectFs (watched, so edits under the tree reload); a
            // .nhproj opens as a live ArchiveProjectFs. --config / --input, if
            // supplied, name the root keys *inside* the project (used verbatim,
            // not resolved against the filesystem); otherwise the user picks roots
            // from the project panel, like a dragged-in folder or archive.
            if (!projectPath.empty()) {
                const std::filesystem::path path_abs{projectPath};
                if (!std::filesystem::exists(path_abs)) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliPathNotFound,
                                            std::format("path not found: {}", projectPath),
                                            projectPath};
                }
                if (std::filesystem::is_directory(path_abs)) {
                    application->setProject(
                        std::make_unique<nodehammer::viewer::WatchedFilesystemProjectFs>(
                            std::make_unique<nodehammer::viewer::FilesystemProjectFs>(path_abs)));
                } else if (isZipPath(path_abs)) {
                    application->setProject(
                        std::make_unique<nodehammer::viewer::ArchiveProjectFs>(path_abs));
                } else {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalCliUsage,
                        std::format("positional path must be a .zip archive or directory: {}",
                                    projectPath),
                        projectPath};
                }
                if (!configPath.empty() && !inputPath.empty()) {
                    application->setRootKeys(configPath, inputPath);
                }
                runViewer(application);
                return;
            }

            // Native CLI flow: if both roots live under the launch CWD, mount that
            // directory as a real filesystem project so the tree can expose
            // neighbouring includes/assets. Otherwise keep the historical bag path:
            // read the supplied files into memory and build from those keys.
            // Existence checks happen here so typos exit before the window opens;
            // build failures show up as red text in the project panel.
            // The web flow lives in src/web/viewer_main.cpp.
            if (!inputPath.empty()) {
                const std::filesystem::path config_path{configPath};
                const std::filesystem::path geometry_path{inputPath};
                if (!configPath.empty() && !std::filesystem::exists(config_path)) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliPathNotFound,
                                            std::format("config file not found: {}", configPath),
                                            configPath};
                }
                if (!std::filesystem::exists(geometry_path)) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliPathNotFound,
                                            std::format("input file not found: {}", inputPath),
                                            inputPath};
                }

                std::error_code path_ec;
                const auto launch_cwd = std::filesystem::current_path(path_ec);
                if (path_ec) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliPathNotFound,
                                            std::format("cannot read current working directory: {}",
                                                        path_ec.message())};
                }
                const auto cwd = std::filesystem::canonical(launch_cwd, path_ec);
                if (path_ec) {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalCliPathNotFound,
                        std::format("cannot resolve current working directory: {}",
                                    path_ec.message())};
                }
                const auto geometry_abs = std::filesystem::canonical(geometry_path, path_ec);
                if (path_ec) {
                    throw nodehammer::Error{nodehammer::codes::kFatalCliPathNotFound,
                                            std::format("cannot resolve input file '{}': {}",
                                                        inputPath, path_ec.message()),
                                            inputPath};
                }

                if (!configPath.empty()) {
                    const auto config_abs = std::filesystem::canonical(config_path, path_ec);
                    if (path_ec) {
                        throw nodehammer::Error{nodehammer::codes::kFatalCliPathNotFound,
                                                std::format("cannot resolve config file '{}': {}",
                                                            configPath, path_ec.message()),
                                                configPath};
                    }
                    if (config_abs == geometry_abs) {
                        throw nodehammer::Error{
                            nodehammer::codes::kFatalCliUsage,
                            std::format("--config and --input point at the same file: {}",
                                        configPath),
                            configPath};
                    }

                    auto config_key = relativeKeyUnder(cwd, config_abs);
                    auto geometry_key = relativeKeyUnder(cwd, geometry_abs);
                    if (config_key && geometry_key) {
                        application->setProject(
                            std::make_unique<nodehammer::viewer::WatchedFilesystemProjectFs>(
                                std::make_unique<nodehammer::viewer::FilesystemProjectFs>(cwd)));
                        application->setRootKeys(std::move(*config_key), std::move(*geometry_key));
                        runViewer(application);
                        return;
                    }
                }

                auto bag = std::make_unique<nodehammer::viewer::BagProjectFs>();

                std::string config_key;
                if (!configPath.empty()) {
                    auto bytes = nodehammer::detail::file_io::readFile(config_path);
                    config_key = config_path.filename().string();
                    bag->addBytes(config_key, std::span<const std::byte>{bytes});
                }
                std::string geometry_key = geometry_path.filename().string();
                if (geometry_key == config_key) {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalCliUsage,
                        std::format("--config and --input point at the same file: {}", configPath),
                        configPath};
                }
                {
                    auto bytes = nodehammer::detail::file_io::readFile(geometry_path);
                    bag->addBytes(geometry_key, std::span<const std::byte>{bytes});
                }

                application->setProject(std::move(bag));
                application->setRootKeys(std::move(config_key), std::move(geometry_key));
            }

            runViewer(application);
        });
    });
}

} // namespace nodehammer::cli::detail
