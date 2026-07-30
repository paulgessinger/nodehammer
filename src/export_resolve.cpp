#include <export_resolve.hpp>

#include <string>
#include <variant>

namespace nodehammer::pipeline {

namespace {

/// Stable key into `NHConfig::exportFormats` for a resolved format.
[[nodiscard]] std::string formatKey(ir::ExportConfig::Format fmt) {
    switch (fmt) {
    case ir::ExportConfig::Format::GLB:
        return "glb";
    case ir::ExportConfig::Format::GLTF:
        return "gltf";
    case ir::ExportConfig::Format::OBJ:
        return "obj";
    }
    return "obj";
}

/// Overlay one `[export.<key>]` table onto `ecfg`. Returns false when the table
/// is absent, which is what drives the GLB→gltf fallback.
bool applyFormatTable(ir::ExportConfig &ecfg, const config::NHConfig &cfg, const std::string &key) {
    if (!cfg.exportFormats.contains(key)) {
        return false;
    }
    const auto &variant = cfg.exportFormats.at(key);
    const auto &common = config::commonConfig(variant);
    if (auto v = common.unitScale) {
        ecfg.unitScale = *v;
    }
    if (const auto *gltfCfg = std::get_if<config::GltfExportFormatConfig>(&variant)) {
        // OBJ has no bake override by construction: the field is glTF/GLB-only,
        // so `defaultBakeUnitScale(OBJ) == true` always stands.
        if (auto v = gltfCfg->bakeUnitScale) {
            ecfg.bakeUnitScale = *v;
        }
        if (auto v = gltfCfg->multiScene) {
            ecfg.gltf.multiScene = *v;
        }
        if (auto v = gltfCfg->sceneNameSeparator) {
            ecfg.gltf.sceneNameSeparator = *v;
        }
    }
    return true;
}

} // namespace

ir::ExportConfig resolveExportConfig(const config::NHConfig &cfg,
                                     const std::filesystem::path &outputPath,
                                     std::string_view formatHint) {
    ir::ExportConfig ecfg;
    ecfg.format = ir::ExportConfig::formatFromExtension(outputPath, formatHint);
    ecfg.unitScale = ir::ExportConfig::defaultUnitScale(ecfg.format);
    ecfg.bakeUnitScale = ir::ExportConfig::defaultBakeUnitScale(ecfg.format);

    const std::string key = formatKey(ecfg.format);
    if (!applyFormatTable(ecfg, cfg, key) && ecfg.format == ir::ExportConfig::Format::GLB) {
        applyFormatTable(ecfg, cfg, "gltf");
    }
    return ecfg;
}

} // namespace nodehammer::pipeline
