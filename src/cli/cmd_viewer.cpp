#include <CLI/CLI.hpp>
#include <nodehammer/detail/file_io.hpp>
#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/bag_project_fs.hpp>
#include <nodehammer/viewer/config.hpp>

#include <filesystem>
#include <memory>
#include <print>
#include <span>
#include <string>

void registerCmdViewer(CLI::App &app) {
    auto *sub = app.add_subcommand("viewer", "Open the interactive 3D viewer");

    auto cfg = std::make_shared<nodehammer::viewer::Config>();
    auto initialCamera = std::make_shared<nodehammer::viewer::Camera>();
    auto cameraYawDeg = std::make_shared<float>(0.f);
    auto cameraPitchDeg = std::make_shared<float>(0.f);
    sub->add_option("--width", cfg->width, "Initial window width in pixels")->capture_default_str();
    sub->add_option("--height", cfg->height, "Initial window height in pixels")
        ->capture_default_str();
    sub->add_option("--title", cfg->title, "Window title")->capture_default_str();
    sub->add_flag("!--no-vsync", cfg->vsync, "Disable vsync (default: vsync on)");
    sub->add_flag("!--no-cull-back", cfg->cull_back, "Disable backface culling")
        ->capture_default_str();
    sub->add_flag("!--no-pause-when-unfocused", cfg->pause_when_unfocused,
                  "Keep rendering when the viewer is unfocused")
        ->capture_default_str();
    sub->add_flag("--auto-orbit", cfg->auto_orbit, "Start with camera auto-orbit enabled")
        ->capture_default_str();
    sub->add_option("--orbit-speed", cfg->auto_orbit_speed_deg, "Auto-orbit speed in degrees/s")
        ->capture_default_str();
    sub->add_flag("--angle-cut", cfg->angle_cut, "Start with angle cut enabled")
        ->capture_default_str();
    sub->add_flag("!--no-shader-angle-cut", cfg->shader_angle_cut, "Disable shader-side angle cut")
        ->capture_default_str();
    sub->add_option("--cut-start", cfg->angle_cut_start_deg, "Angle cut start in degrees")
        ->capture_default_str();
    sub->add_option("--cut-end", cfg->angle_cut_end_deg, "Angle cut end in degrees")
        ->capture_default_str();
    sub->add_flag("!--no-pbr", cfg->enable_pbr, "Disable PBR/IBL shading")->capture_default_str();
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

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file (.nhb / .nhb.zst)");
    auto *configOpt = sub->add_option("-c,--config", "TOML config file");

    sub->callback([cfg, initialCamera, cameraYawDeg, cameraPitchDeg, cameraTargetXOpt,
                   cameraTargetYOpt, cameraTargetZOpt, cameraDistanceOpt, cameraYawOpt,
                   cameraPitchOpt, inputOpt, configOpt]() {
        const bool hasCameraOption = *cameraTargetXOpt || *cameraTargetYOpt || *cameraTargetZOpt ||
                                     *cameraDistanceOpt || *cameraYawOpt || *cameraPitchOpt;
        const bool hasAllCameraOptions = *cameraTargetXOpt && *cameraTargetYOpt &&
                                         *cameraTargetZOpt && *cameraDistanceOpt && *cameraYawOpt &&
                                         *cameraPitchOpt;
        if (hasCameraOption && !hasAllCameraOptions) {
            std::println(
                stderr,
                "viewer: camera URL restore requires target x/y/z, distance, yaw, and pitch");
            std::exit(1);
        }
        if (hasAllCameraOptions) {
            if (initialCamera->distance <= 0.f) {
                std::println(stderr, "viewer: --camera-distance must be positive");
                std::exit(1);
            }
            initialCamera->yaw = glm::radians(*cameraYawDeg);
            initialCamera->pitch = glm::radians(*cameraPitchDeg);
            cfg->initial_camera = *initialCamera;
        }

        std::string inputPath, configPath;
        if (*inputOpt) {
            inputOpt->results(inputPath);
        }
        if (*configOpt) {
            configOpt->results(configPath);
        }

        nodehammer::viewer::App::Handle application(*cfg);

        // Native CLI flow: if --input is supplied, read both files into a
        // BagProjectFs and let the App's BuildSession drive the build
        // asynchronously after the window opens (same path URL mode and
        // drag-drop go through). Existence checks happen here so typos
        // exit before the window opens; build failures show up as red
        // text in the project panel.
        // The web flow lives in src/web/viewer_main.cpp.
        if (!inputPath.empty()) {
            const std::filesystem::path config_path{configPath};
            const std::filesystem::path geometry_path{inputPath};
            if (!configPath.empty() && !std::filesystem::exists(config_path)) {
                std::println(stderr, "viewer: config file not found: {}", configPath);
                std::exit(1);
            }
            if (!std::filesystem::exists(geometry_path)) {
                std::println(stderr, "viewer: input file not found: {}", inputPath);
                std::exit(1);
            }

            auto bag = std::make_unique<nodehammer::viewer::BagProjectFs>();

            std::string config_key;
            if (!configPath.empty()) {
                auto bytes = nodehammer::file_io::readFile(config_path);
                config_key = config_path.filename().string();
                bag->addBytes(config_key, std::span<const std::byte>{bytes});
            }
            std::string geometry_key = geometry_path.filename().string();
            if (geometry_key == config_key) {
                std::println(stderr, "viewer: --config and --input point at the same file: {}",
                             configPath);
                std::exit(1);
            }
            {
                auto bytes = nodehammer::file_io::readFile(geometry_path);
                bag->addBytes(geometry_key, std::span<const std::byte>{bytes});
            }

            application->setProject(std::move(bag));
            application->setRootKeys(std::move(config_key), std::move(geometry_key));
        }

        const int rc = application->run();
        if (rc != 0) {
            std::println(stderr, "viewer exited with code {}", rc);
        }
    });
}
