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

/// Does this path name an archive project?
///
/// `.nhproj` is the extension `project pack` writes and the one the drop
/// handler recognises; `.zip` is what the container actually is, and predates
/// the name. Both open the same `ArchiveProjectFs`, so both are accepted here
/// rather than making the CLI the one place that refuses its own format.
bool isArchivePath(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".nhproj" || ext == ".zip";
}

/// Which of the three window-backed modes is running.
///
/// They were flags on one command: `--screenshot PATH` turned "open a window"
/// into "render and quit", and `--bench PATH` into "measure and quit". A flag
/// that changes what the command *is* reads as an option on the thing it
/// replaces, so they are modes now, and this is what tells the shared body which
/// one it is serving.
enum class NativeMode { Open, Shot, Bench };

/// The window options, and the handles needed to tell whether each was given.
///
/// Declared per mode rather than on `viewer` itself. On the parent they would
/// also parse under `serve`, where a window option means nothing -- which is the
/// state `refuseNativeOnlyOptions` used to paper over with a hand-maintained
/// allowlist. Registering them only where they apply lets the parser refuse
/// them, and the parser cannot fall out of step with the list.
struct WindowOptions {
    CLI::Option *cullModeOpt = nullptr;
    CLI::Option *pauseWhenUnfocusedOpt = nullptr;
    CLI::Option *autoOrbitOpt = nullptr;
    CLI::Option *orbitSpeedOpt = nullptr;
    CLI::Option *angleCutOpt = nullptr;
    CLI::Option *shaderAngleCutOpt = nullptr;
    CLI::Option *cutStartOpt = nullptr;
    CLI::Option *cutEndOpt = nullptr;
    CLI::Option *pbrOpt = nullptr;
    CLI::Option *cameraTargetXOpt = nullptr;
    CLI::Option *cameraTargetYOpt = nullptr;
    CLI::Option *cameraTargetZOpt = nullptr;
    CLI::Option *cameraDistanceOpt = nullptr;
    CLI::Option *cameraYawOpt = nullptr;
    CLI::Option *cameraPitchOpt = nullptr;
};

/// Add the window and camera options to one mode.
///
/// The bound `cfg`/`camera` are shared across all three, which is safe because
/// exactly one mode ever runs: CLI11 parses at most one subcommand here, so only
/// that mode's options were ever written.
WindowOptions addWindowOptions(CLI::App &sub,
                               const std::shared_ptr<nodehammer::viewer::Config> &cfg,
                               const std::shared_ptr<nodehammer::viewer::Camera> &camera,
                               const std::shared_ptr<float> &yawDeg,
                               const std::shared_ptr<float> &pitchDeg,
                               const std::shared_ptr<std::string> &cullModeStr) {
    WindowOptions win;
    sub.add_option("--width", cfg->width, "Initial window width in pixels")->capture_default_str();
    sub.add_option("--height", cfg->height, "Initial window height in pixels")
        ->capture_default_str();
    sub.add_flag("!--no-vsync", cfg->vsync, "Disable vsync (default: vsync on)");
    win.cullModeOpt =
        sub.add_option("--cull", *cullModeStr,
                       "Backface cull override: auto (per material), force-on, force-off")
            ->capture_default_str();
    win.pauseWhenUnfocusedOpt =
        sub.add_flag("!--no-pause-when-unfocused", cfg->pause_when_unfocused,
                     "Keep rendering when the viewer is unfocused")
            ->capture_default_str();
    win.autoOrbitOpt =
        sub.add_flag("--auto-orbit", cfg->auto_orbit, "Start with camera auto-orbit enabled")
            ->capture_default_str();
    win.orbitSpeedOpt =
        sub.add_option("--orbit-speed", cfg->auto_orbit_speed_deg, "Auto-orbit speed in degrees/s")
            ->capture_default_str();
    // `--angle-cut` here is the shader effect on the view. `convert` has a
    // wedge cut, which rebuilds geometry; they used to share this name.
    win.angleCutOpt = sub.add_flag("--angle-cut", cfg->angle_cut, "Start with angle cut enabled")
                          ->capture_default_str();
    win.shaderAngleCutOpt = sub.add_flag("!--no-shader-angle-cut", cfg->shader_angle_cut,
                                         "Disable shader-side angle cut")
                                ->capture_default_str();
    win.cutStartOpt =
        sub.add_option("--cut-start", cfg->angle_cut_start_deg, "Angle cut start in degrees")
            ->capture_default_str();
    win.cutEndOpt = sub.add_option("--cut-end", cfg->angle_cut_end_deg, "Angle cut end in degrees")
                        ->capture_default_str();
    win.pbrOpt = sub.add_flag("!--no-pbr", cfg->enable_pbr, "Disable PBR/IBL shading")
                     ->capture_default_str();
    win.cameraTargetXOpt =
        sub.add_option("--camera-target-x", camera->target.x, "Initial camera target X coordinate");
    win.cameraTargetYOpt =
        sub.add_option("--camera-target-y", camera->target.y, "Initial camera target Y coordinate");
    win.cameraTargetZOpt =
        sub.add_option("--camera-target-z", camera->target.z, "Initial camera target Z coordinate");
    win.cameraDistanceOpt =
        sub.add_option("--camera-distance", camera->distance, "Initial camera orbit distance");
    win.cameraYawOpt = sub.add_option("--camera-yaw", *yawDeg, "Initial camera yaw in degrees");
    win.cameraPitchOpt =
        sub.add_option("--camera-pitch", *pitchDeg, "Initial camera pitch in degrees");
    return win;
}

} // namespace

namespace nodehammer::cli::detail {

// `options` is unnamed: the native modes read every setting they need off the
// parser, and the one front-door property that is not on the command line --
// `webAssets` -- belongs to `serve`, which lives in the library half. The
// parameter stays so both registrars are called the same way from `main`.
void registerCmdViewerNative(CLI::App &app, const RunOptions & /*options*/) {
    // Fills in, does not create. `registerCmdViewer` in the library owns the
    // `viewer` subcommand, its shared options and `serve`, because those have to
    // exist in builds this file is not compiled into -- a wheel above all. It
    // also declares `open`, `shot` and `bench` as stubs that refuse, so that a
    // wheel answers them with a message naming `serve` instead of CLI11's
    // "unexpected argument". What is left here is the window: the options only a
    // window has, and the bodies that replace those refusals.
    CLI::App *viewer = app.get_subcommand("viewer");
    CLI::App *openSub = viewer->get_subcommand("open");
    CLI::App *shotSub = viewer->get_subcommand("shot");
    CLI::App *benchSub = viewer->get_subcommand("bench");

    auto cfg = std::make_shared<nodehammer::viewer::Config>();
    auto initialCamera = std::make_shared<nodehammer::viewer::Camera>();
    auto cameraYawDeg = std::make_shared<float>(0.f);
    auto cameraPitchDeg = std::make_shared<float>(0.f);
    auto cullModeStr = std::make_shared<std::string>("auto");

    const auto openWin =
        addWindowOptions(*openSub, cfg, initialCamera, cameraYawDeg, cameraPitchDeg, cullModeStr);
    const auto shotWin =
        addWindowOptions(*shotSub, cfg, initialCamera, cameraYawDeg, cameraPitchDeg, cullModeStr);
    const auto benchWin =
        addWindowOptions(*benchSub, cfg, initialCamera, cameraYawDeg, cameraPitchDeg, cullModeStr);

    // ── shot ─────────────────────────────────────────────────────────────────
    //
    // Renders one high-res PNG once the scene settles, then quits. `-o` rather
    // than the old `--screenshot PATH`: the destination is this mode's output,
    // and every other command in the tree spells that `-o`.
    auto screenshot = std::make_shared<nodehammer::viewer::PngExportSettings>();
    auto *shotOutOpt =
        shotSub->add_option("-o,--output", "PNG to write")->required()->type_name("FILE");
    shotSub->add_option("--shot-width", screenshot->out_width, "Screenshot output width")
        ->capture_default_str();
    shotSub->add_option("--shot-height", screenshot->out_height, "Screenshot output height")
        ->capture_default_str();
    shotSub
        ->add_option("--supersample", screenshot->supersample,
                     "Screenshot supersampling factor (1-4)")
        ->capture_default_str();

    // ── bench ────────────────────────────────────────────────────────────────
    //
    // Drives a fixed camera/state sequence (cut-on hold/orbit/zoom, then
    // cut-off), measures per-pass GPU time over each window, grabs a screenshot
    // per segment, writes the results JSON, then quits. D3D11 only for the GPU
    // timings.
    auto *benchOutOpt =
        benchSub->add_option("-o,--output", "Results JSON to write")->required()->type_name("FILE");
    auto benchScale = std::make_shared<float>(1.0f);
    benchSub
        ->add_option("--scale", *benchScale,
                     "Bench SSAA on the scene/AO passes (render_scale, 0.25-4). >1 pushes the "
                     "GPU past the refresh so total/composite timings aren't present-paced")
        ->capture_default_str();

    // The one body all three share. They differ in what they ask the App for
    // before it runs, and in nothing else -- the scene, the project and every
    // window option are resolved the same way whether a person is going to look
    // at the result or a file is.
    const auto runNative = [viewer, cfg, initialCamera, cameraYawDeg, cameraPitchDeg, cullModeStr,
                            screenshot, benchScale](NativeMode mode, const WindowOptions &win,
                                                    const std::string &outPath) {
        // The only command that never had one. Every other `cmd_*.cpp` has run
        // its body through this since the error model landed; this one instead
        // validated by hand and called `std::exit`, which meant a
        // `file_io::readFile` throw below had no handler at all and reached
        // `std::terminate`.
        runOrReport("viewer", [&] {
            if (*win.cullModeOpt) {
                using nodehammer::viewer::CullOverride;
                CullOverride cull = CullOverride::Auto;
                if (*cullModeStr == "force-on") {
                    cull = CullOverride::ForceCull;
                } else if (*cullModeStr == "force-off") {
                    cull = CullOverride::ForceNoCull;
                } else if (*cullModeStr != "auto") {
                    throw CLI::ValidationError("--cull",
                                               "must be one of: auto, force-on, force-off");
                }
                cfg->cull = cull;
                cfg->startup_overrides.cull = cull;
            }
            if (*win.pauseWhenUnfocusedOpt) {
                cfg->startup_overrides.pause_when_unfocused = cfg->pause_when_unfocused;
            }
            if (*win.autoOrbitOpt) {
                cfg->startup_overrides.auto_orbit = cfg->auto_orbit;
            }
            if (*win.orbitSpeedOpt) {
                cfg->startup_overrides.auto_orbit_speed_deg = cfg->auto_orbit_speed_deg;
            }
            if (*win.angleCutOpt) {
                cfg->startup_overrides.angle_cut = cfg->angle_cut;
            }
            if (*win.shaderAngleCutOpt) {
                cfg->startup_overrides.shader_angle_cut = cfg->shader_angle_cut;
            }
            if (*win.cutStartOpt) {
                cfg->startup_overrides.angle_cut_start_deg = cfg->angle_cut_start_deg;
            }
            if (*win.cutEndOpt) {
                cfg->startup_overrides.angle_cut_end_deg = cfg->angle_cut_end_deg;
            }
            if (*win.pbrOpt) {
                cfg->startup_overrides.enable_pbr = cfg->enable_pbr;
            }

            const bool hasCameraOption = *win.cameraTargetXOpt || *win.cameraTargetYOpt ||
                                         *win.cameraTargetZOpt || *win.cameraDistanceOpt ||
                                         *win.cameraYawOpt || *win.cameraPitchOpt;
            const bool hasAllCameraOptions = *win.cameraTargetXOpt && *win.cameraTargetYOpt &&
                                             *win.cameraTargetZOpt && *win.cameraDistanceOpt &&
                                             *win.cameraYawOpt && *win.cameraPitchOpt;
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
            const auto text = [viewer](const char *name) {
                const CLI::Option *opt = viewer->get_option(name);
                return opt->count() > 0 ? opt->as<std::string>() : std::string{};
            };
            const std::string inputPath = text("--input");
            const std::string configPath = text("--config");
            std::string projectPath = text("path");
            if (const std::string title = text("--title"); !title.empty()) {
                cfg->title = title;
            }

            // A mode that renders and quits has nothing to show if there is no
            // scene, and no window in which to say so afterwards.
            if (mode != NativeMode::Open && inputPath.empty()) {
                throw nodehammer::Error{
                    nodehammer::codes::kFatalCliUsage,
                    std::format("`viewer {}` needs --input: there is no scene to render",
                                mode == NativeMode::Shot ? "shot" : "bench")};
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
            if (mode == NativeMode::Shot) {
                application->requestScreenshot(outPath, *screenshot);
            } else if (mode == NativeMode::Bench) {
                application->requestBench(outPath, inputPath, *benchScale);
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
                } else if (isArchivePath(path_abs)) {
                    application->setProject(
                        std::make_unique<nodehammer::viewer::ArchiveProjectFs>(path_abs));
                } else {
                    throw nodehammer::Error{
                        nodehammer::codes::kFatalCliUsage,
                        std::format("positional path must be a .nhproj archive or a "
                                    "directory: {}",
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
    };

    openSub->callback([=] { runNative(NativeMode::Open, openWin, {}); });
    shotSub->callback([=] {
        std::string out;
        shotOutOpt->results(out);
        runNative(NativeMode::Shot, shotWin, out);
    });
    benchSub->callback([=] {
        std::string out;
        benchOutOpt->results(out);
        runNative(NativeMode::Bench, benchWin, out);
    });

    // A bare `nodehammer viewer` opens a window, which is what a .desktop
    // `Exec=` or an installer shortcut invokes (#74). It replaces the library's
    // refusal rather than adding to it.
    viewer->callback([=] {
        if (viewer->get_subcommands().empty()) {
            runNative(NativeMode::Open, openWin, {});
        }
    });
}

} // namespace nodehammer::cli::detail
