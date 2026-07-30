#include "cli_common.hpp"

#include <CLI/CLI.hpp>
#include <config/config_ast.hpp>
#include <config/config_loader.hpp>
#include <detail/zstd_io.hpp>
#include <ir/render_json.hpp>
#include <ir/synthetic/semantic/importer.hpp>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <tessellation/tessellation_pass.hpp>

void registerCmdDumpRender(CLI::App &app) {
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
        nodehammer::config::NHConfig cfg;
        if (*configOpt) {
            std::string cfgPath;
            configOpt->results(cfgPath);
            auto loaded = nodehammer::config::ConfigLoader::loadFromFile(cfgPath);
            nodehammer::cli::printDiags(loaded.diags);
            if (loaded.diags.hasErrors()) {
                return;
            }
            cfg = std::move(loaded.config);
        }

        // ── Import scene ───────────────────────────────────────────────────────
        nodehammer::ir::SemanticScene semScene;
        if (syntheticBoxOpt->count()) {
            semScene = nodehammer::ir::SyntheticSceneBuilder::buildSingleBox();
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
        nodehammer::tessellation::TessellationPass pass{cfg};
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
