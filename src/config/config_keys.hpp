#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

// Canonical config key names, in one place.
//
// The TOML loader (config_loader.cpp), the TOML writer (config_writer.cpp) and
// the Lua front-end (lua_config.cpp) all describe the same schema. To keep those
// paths from drifting — a key renamed in one, a new field added to two of the
// three — the *set of valid keys per section* lives here as the single source of
// truth. Each front-end validates unknown keys against these lists, so a typo or
// a missing key surfaces as a diagnostic on both paths rather than silently.
namespace nodehammer::config::keys {

// ── Top-level sections ───────────────────────────────────────────────────────
inline constexpr std::string_view kHoistOrphans = "hoist_orphans";
inline constexpr std::string_view kDeduplicateShapes = "deduplicate_shapes";
inline constexpr std::string_view kExport = "export";
inline constexpr std::string_view kMaterials = "materials";
inline constexpr std::string_view kSelectionRules = "selection_rules";
inline constexpr std::string_view kRules = "rules";
inline constexpr std::string_view kDefaults = "defaults";

inline constexpr std::array kTopLevelKeys = {
    kHoistOrphans, kDeduplicateShapes, kExport, kMaterials, kSelectionRules, kRules, kDefaults,
};

// The two flags the Lua `config{…}` primitive accepts (the TOML loader reads
// these as top-level scalars; the Lua DSL groups them under config{}).
inline constexpr std::array kConfigFlagKeys = {kHoistOrphans, kDeduplicateShapes};

// ── [materials.<name>] ───────────────────────────────────────────────────────
inline constexpr std::string_view kBaseColor = "base_color";
inline constexpr std::string_view kMetallic = "metallic";
inline constexpr std::string_view kRoughness = "roughness";
inline constexpr std::string_view kDoubleSided = "double_sided";
inline constexpr std::string_view kEmissive = "emissive";
inline constexpr std::string_view kAlphaMode = "alpha_mode";
inline constexpr std::string_view kAlphaCutoff = "alpha_cutoff";
inline constexpr std::string_view kIor = "ior";
inline constexpr std::string_view kTransmission = "transmission";
inline constexpr std::string_view kClearcoat = "clearcoat";
inline constexpr std::string_view kClearcoatRoughness = "clearcoat_roughness";
inline constexpr std::string_view kAnisotropy = "anisotropy";
inline constexpr std::string_view kAnisotropyRotation = "anisotropy_rotation";
inline constexpr std::string_view kSpecular = "specular";
inline constexpr std::string_view kSpecularColor = "specular_color";

inline constexpr std::array kMaterialKeys = {
    kBaseColor,          kMetallic, kRoughness,     kDoubleSided, kEmissive,           kAlphaMode,
    kAlphaCutoff,        kIor,      kTransmission,  kClearcoat,   kClearcoatRoughness, kAnisotropy,
    kAnisotropyRotation, kSpecular, kSpecularColor,
};

// ── [[selection_rules]] ──────────────────────────────────────────────────────
inline constexpr std::string_view kKeepIf = "keep_if";
inline constexpr std::string_view kDropIf = "drop_if";
inline constexpr std::string_view kScope = "scope";

inline constexpr std::array kSelectionRuleKeys = {kKeepIf, kDropIf, kScope};

// ── [[rules]] ────────────────────────────────────────────────────────────────
inline constexpr std::string_view kMatch = "match";
inline constexpr std::string_view kMaterialRef = "material";
inline constexpr std::string_view kTessellation = "tessellation";
inline constexpr std::string_view kExtras = "extras";

inline constexpr std::array kRuleKeys = {kMatch, kMaterialRef, kTessellation, kExtras};

// ── [rules.tessellation] / [defaults.tessellation] ───────────────────────────
inline constexpr std::string_view kSkipGeometry = "skip_geometry";
inline constexpr std::string_view kMergeDescendants = "merge_descendants";
inline constexpr std::string_view kDropCoincidentFaces = "drop_coincident_faces";
inline constexpr std::string_view kAverageMaterialStack = "average_material_stack";
inline constexpr std::string_view kMaxSegmentsCircle = "max_segments_circle";
inline constexpr std::string_view kFallback = "fallback";

inline constexpr std::array kTessellationKeys = {
    kSkipGeometry,         kMergeDescendants,  kDropCoincidentFaces,
    kAverageMaterialStack, kMaxSegmentsCircle, kFallback,
};

// ── [defaults] ───────────────────────────────────────────────────────────────
inline constexpr std::array kDefaultsKeys = {kTessellation, kExtras};

// ── [export.<fmt>] ───────────────────────────────────────────────────────────
inline constexpr std::string_view kUnitScale = "unit_scale";
inline constexpr std::string_view kBakeUnitScale = "bake_unit_scale";           // gltf/glb only
inline constexpr std::string_view kMultiScene = "multi_scene";                  // gltf/glb only
inline constexpr std::string_view kSceneNameSeparator = "scene_name_separator"; // gltf/glb only

inline constexpr std::string_view kGltfGlbFormats[] = {"gltf", "glb"};

/// Which `[export.<fmt>]` keys each format accepts.
///
/// This is the schema itself, not a per-front-end allowlist: both the TOML
/// loader and the Lua builder walk this table, so a format-specific key cannot
/// be right in one front-end and wrong in the other. Adding a key here is the
/// whole registration step.
///
/// An empty `formats` means every format accepts the key. A key used outside its
/// formats is *misplaced* rather than unknown, and both front-ends say so
/// precisely — each in its own syntax, which is why only the data is shared and
/// the message is not.
struct ExportKeyDef {
    std::string_view key;
    std::span<const std::string_view> formats;
};

inline constexpr ExportKeyDef kExportKeys[] = {
    {kUnitScale, {}},
    {kBakeUnitScale, kGltfGlbFormats},
    {kMultiScene, kGltfGlbFormats},
    {kSceneNameSeparator, kGltfGlbFormats},
};

[[nodiscard]] inline bool exportKeyAllowed(const ExportKeyDef &kd, std::string_view fmt) {
    return kd.formats.empty() ||
           std::find(kd.formats.begin(), kd.formats.end(), fmt) != kd.formats.end();
}

} // namespace nodehammer::config::keys
