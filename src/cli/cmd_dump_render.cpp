#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/detail/zstd_io.hpp>
#include <nodehammer/import/synthetic.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/tessellation/tessellation_pass.hpp>
#include <print>
#include <string>

void register_cmd_dump_render(CLI::App &app) {
    auto *sub = app.add_subcommand("dump-render", "Dump the render IR of a geometry as JSON");

    auto *inputOpt = sub->add_option("-i,--input", "Input geometry file");
    auto *formatOpt =
        sub->add_option("--input-format", "Input format (auto-detected from extension if omitted)");
    auto *configOpt = sub->add_option("-c,--config", "TOML config file");
    auto *outputOpt = sub->add_option("-o,--output", "Output JSON file (default: stdout)");
    auto *syntheticBoxOpt =
        sub->add_flag("--synthetic-box", "Use a synthetic single-box scene as input");

    sub->callback([=] {
        // ── Load config ────────────────────────────────────────────────────────
        nodehammer::NHConfig cfg;
        if (*configOpt) {
            std::string cfgPath;
            configOpt->results(cfgPath);
            auto loaded = nodehammer::ConfigLoader::loadFromFile(cfgPath);
            nodehammer::cli::printDiags(loaded.diags);
            if (loaded.diags.hasErrors()) {
                return;
            }
            cfg = std::move(loaded.config);
        }

        // ── Import scene ───────────────────────────────────────────────────────
        nodehammer::SemanticScene semScene;
        if (syntheticBoxOpt->count()) {
            semScene = nodehammer::SyntheticSceneBuilder::buildSingleBox();
        } else if (*inputOpt) {
            auto [importResult, fmt] = nodehammer::cli::importOrExit(inputOpt, formatOpt);
            nodehammer::cli::printDiags(importResult.diags);
            if (importResult.diags.hasErrors())
                return;
            semScene = std::move(importResult.scene);
        } else {
            std::println(stderr,
                         "nodehammer dump-render: specify --input <file> or --synthetic-box");
            return;
        }

        // ── Deduplicate shapes ─────────────────────────────────────────────────
        if (cfg.deduplicateShapes) {
            semScene.deduplicateMaterials();
            semScene.deduplicateShapes();
            semScene.deduplicateLogVols();
        }

        // ── Tessellate ─────────────────────────────────────────────────────────
        nodehammer::TessellationPass pass{cfg};
        auto passResult = pass.lower(semScene);
        nodehammer::cli::printDiags(passResult.diags);

        // ── Serialize ──────────────────────────────────────────────────────────
        nlohmann::json j = passResult.scene;
        std::string jsonStr = j.dump(2);
        std::string outPath;
        if (*outputOpt) {
            outputOpt->results(outPath);
            nodehammer::zstd_io::writeJsonToFile(outPath, jsonStr);
        } else {
            std::println("{}", jsonStr);
        }
    });
}
