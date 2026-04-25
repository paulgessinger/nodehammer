#include <CLI/CLI.hpp>
#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/config.hpp>

#include <print>

void register_cmd_viewer(CLI::App &app) {
    auto *sub =
        app.add_subcommand("viewer", "Open the interactive 3D viewer (Stage 1: hello triangle)");

    auto cfg = std::make_shared<nodehammer::viewer::Config>();
    sub->add_option("--width", cfg->width, "Initial window width in pixels")->capture_default_str();
    sub->add_option("--height", cfg->height, "Initial window height in pixels")
        ->capture_default_str();
    sub->add_option("--title", cfg->title, "Window title")->capture_default_str();
    sub->add_flag("!--no-vsync", cfg->vsync, "Disable vsync (default: vsync on)");

    sub->callback([cfg]() {
        nodehammer::viewer::App application(*cfg);
        const int rc = application.run();
        if (rc != 0) {
            std::println(stderr, "viewer exited with code {}", rc);
        }
    });
}
