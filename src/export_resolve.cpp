#include <nodehammer/export_resolve.hpp>

#include <string>
#include <variant>

namespace nodehammer {

namespace {

/// Stable key into `NHConfig::exportFormats` for a resolved format.
[[nodiscard]] std::string formatKey(ExportConfig::Format fmt) {
    switch (fmt) {
    case ExportConfig::Format::GLB:
        return "glb";
    case ExportConfig::Format::GLTF:
        return "gltf";
    case ExportConfig::Format::OBJ:
        return "obj";
    }
    return "obj";
}

/// Overlay one `[export.<key>]` table onto `ecfg`. Returns false when the table
/// is absent, which is what drives the GLB→gltf fallback.
bool applyFormatTable(ExportConfig &ecfg, const NHConfig &cfg, const std::string &key) {
    if (!cfg.exportFormats.contains(key)) {
        return false;
    }
    const auto &variant = cfg.exportFormats.at(key);
    const auto &common = commonConfig(variant);
    if (auto v = common.unitScale) {
        ecfg.unitScale = *v;
    }
    if (const auto *gltfCfg = std::get_if<GltfExportFormatConfig>(&variant)) {
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

ExportConfig resolveExportConfig(const NHConfig &cfg, const std::filesystem::path &outputPath,
                                 std::string_view formatHint) {
    ExportConfig ecfg;
    ecfg.format = ExportConfig::formatFromExtension(outputPath, formatHint);
    ecfg.unitScale = ExportConfig::defaultUnitScale(ecfg.format);
    ecfg.bakeUnitScale = ExportConfig::defaultBakeUnitScale(ecfg.format);

    const std::string key = formatKey(ecfg.format);
    if (!applyFormatTable(ecfg, cfg, key) && ecfg.format == ExportConfig::Format::GLB) {
        applyFormatTable(ecfg, cfg, "gltf");
    }
    return ecfg;
}

} // namespace nodehammer
