#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_validator.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/selection/selector.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/config.hpp>

#include <memory>
#include <print>

using nodehammer::cli::printDiags;

void register_cmd_viewer(CLI::App &app) {
    auto *sub = app.add_subcommand("viewer", "Open the interactive 3D viewer");

    auto cfg = std::make_shared<nodehammer::viewer::Config>();
    sub->add_option("--width", cfg->width, "Initial window width in pixels")->capture_default_str();
    sub->add_option("--height", cfg->height, "Initial window height in pixels")
        ->capture_default_str();
    sub->add_option("--title", cfg->title, "Window title")->capture_default_str();
    sub->add_flag("!--no-vsync", cfg->vsync, "Disable vsync (default: vsync on)");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file (semantic IR)");
    auto *fmtInOpt =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *configOpt = sub->add_option("-c,--config", "TOML config file");

    sub->callback([cfg, inputOpt, fmtInOpt, configOpt]() {
        // Build the scene first (in CPU memory) so the viewer either gets a
        // valid RenderScene or no scene at all (falls back to demo triangle).
        std::shared_ptr<nodehammer::RenderScene> scene;

        if (*inputOpt) {
            nodehammer::NHConfig nhcfg;
            if (*configOpt) {
                std::string cfgPath;
                configOpt->results(cfgPath);
                auto loaded = nodehammer::ConfigLoader::loadFromFile(cfgPath);
                printDiags(loaded.diags);
                if (loaded.diags.hasErrors()) {
                    std::println(stderr, "viewer: config load failed");
                    std::exit(1);
                }
                nhcfg = std::move(loaded.config);
                auto validDiags = nodehammer::ConfigValidator::validate(nhcfg);
                printDiags(validDiags);
                if (validDiags.hasErrors()) {
                    std::println(stderr, "viewer: config validation failed");
                    std::exit(1);
                }
            }

            auto [importResult, importFmt] = nodehammer::cli::importOrExit(inputOpt, fmtInOpt);
            printDiags(importResult.diags);
            if (importResult.diags.hasErrors()) {
                std::println(stderr, "viewer: import failed");
                std::exit(1);
            }

            if (!nhcfg.selection.empty()) {
                nodehammer::SelectionEngine sel{nhcfg.selection, nhcfg.hoistOrphans};
                auto selDiags = sel.prune(importResult.scene);
                printDiags(selDiags);
                if (selDiags.hasErrors()) {
                    std::println(stderr, "viewer: selection failed");
                    std::exit(1);
                }
            }

            if (nhcfg.deduplicateShapes) {
                importResult.scene.deduplicateMaterials();
                importResult.scene.deduplicateShapes();
                importResult.scene.deduplicateLogVols();
            }

            nodehammer::TessellationPass pass{nhcfg};
            auto tessResult = pass.lower(importResult.scene);
            printDiags(tessResult.diags);
            if (tessResult.diags.hasErrors()) {
                std::println(stderr, "viewer: tessellation failed");
                std::exit(1);
            }
            scene = std::make_shared<nodehammer::RenderScene>(std::move(tessResult.scene));
            std::println(stderr, "viewer: loaded {} nodes, {} mesh assets, {} materials",
                         scene->nodes.size(), scene->meshAssets.size(), scene->materials.size());
        }

        nodehammer::viewer::App application(*cfg);
        if (scene) {
            application.set_scene(std::move(scene));
        }
        const int rc = application.run();
        if (rc != 0) {
            std::println(stderr, "viewer exited with code {}", rc);
        }
    });
}
