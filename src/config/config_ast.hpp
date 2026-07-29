#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nodehammer {

// ── Enums ─────────────────────────────────────────────────────────────────────

enum class SelectionAction { KeepIf, DropIf };
enum class BooleanFallback { Skip, BBox, Fail };

// ── Predicate AST ─────────────────────────────────────────────────────────────
// Compound predicates are held via shared_ptr so PredicateExpr stays copyable.
//
// TOML representation uses structured inline tables. String expression predicates
// (e.g. "semantic == 'sensor'") would require a mini-parser and are deferred.

struct NameGlobPredicate {
    std::string pattern;
};

struct PathGlobPredicate {
    std::string pattern;
};

struct MaterialGlobPredicate {
    std::string pattern;
};

/// True when a node carries the given tag key, and optionally a specific value.
/// value = nullopt means "key exists"; value = "x" means "key == 'x'".
struct TagPredicate {
    std::string key;
    std::optional<std::string> value;
};

/// True only for nodes that have no children in the scene.
struct IsLeafPredicate {};

/// Always true / always false.
struct BoolPredicate {
    bool value;
};

struct AndPredicate;
struct OrPredicate;
struct NotPredicate;

using PredicateVariant =
    std::variant<NameGlobPredicate, PathGlobPredicate, MaterialGlobPredicate, TagPredicate,
                 IsLeafPredicate, BoolPredicate, std::shared_ptr<AndPredicate>,
                 std::shared_ptr<OrPredicate>, std::shared_ptr<NotPredicate>>;

struct PredicateExpr {
    PredicateVariant data;
};

struct AndPredicate {
    std::vector<PredicateExpr> operands;
};

struct OrPredicate {
    std::vector<PredicateExpr> operands;
};

struct NotPredicate {
    PredicateExpr operand;
};

// ── Color ─────────────────────────────────────────────────────────────────────

struct Color {
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};
};

// ── Material definition ───────────────────────────────────────────────────────
// In TOML: [materials.<name>] — the key becomes MaterialDef::name.

enum class AlphaMode { Opaque, Mask, Blend };

struct MaterialDef {
    std::string name; ///< Populated from the TOML table key
    Color baseColor{0.8f, 0.8f, 0.8f, 1.0f};
    float metallic{0.0f};
    float roughness{0.5f};
    Color emissive{0.0f, 0.0f, 0.0f, 1.0f};
    // Single-sided by default — closed solids cull their back-faces (see
    // RenderMaterial::doubleSided). Set `double_sided = true` per material to
    // opt into two-sided rendering.
    bool doubleSided{false};
    AlphaMode alphaMode{AlphaMode::Opaque};
    float alphaCutoff{0.5f};
    std::optional<float> ior;          ///< KHR_materials_ior
    std::optional<float> transmission; ///< KHR_materials_transmission
    std::optional<float> clearcoat;    ///< KHR_materials_clearcoat
    std::optional<float> clearcoatRoughness;
    std::optional<float> anisotropy;         ///< KHR_materials_anisotropy
    std::optional<float> anisotropyRotation; ///< radians
    std::optional<float> specularFactor;     ///< KHR_materials_specular
    std::optional<Color> specularColor;
};

// ── Rules ─────────────────────────────────────────────────────────────────────
// In TOML: [[selection_rules]] with keys keep_if or drop_if (not action = "...").
// scope is an optional path glob that restricts the rule to a subtree.

struct SelectionRule {
    SelectionAction action{SelectionAction::KeepIf};
    std::optional<std::string> scope; ///< Optional path glob pre-filter
    PredicateExpr predicate{NameGlobPredicate{"*"}};
};

/// Free-form extras for export metadata (e.g. glTF extras).
/// Uses nlohmann::json as a recursive value type — maps directly to tinygltf::Value.
using ExtrasMap = nlohmann::json;

/// Unified rule: optional material, tessellation, and/or extras on matched nodes.
/// In TOML: [[rules]] with match predicate and sub-tables.
struct Rule {
    std::optional<PredicateExpr> match; ///< Optional predicate — omit to match all nodes

    // ── Concerns (all optional; a rule may set any combination) ──────────
    std::optional<std::string> material; ///< References a MaterialDef::name

    struct Tessellation {
        std::optional<bool> skipGeometry;
        std::optional<bool> mergeDescendants;
        // Opt-in post-pass on a merge_descendants group: removes exact-coincident,
        // opposite-wound triangle pairs (the internal interfaces between stacked
        // opaque slabs, e.g. sampling-calorimeter absorber/scintillator layers)
        // before the merged mesh is emitted. Only meaningful together with
        // mergeDescendants — see tessellation_pass.cpp's coincident-face removal
        // helper for the exact algorithm. Valid only for fully-opaque geometry:
        // it assumes the interior faces can never be seen, which holds for uncut
        // and Boolean-cut views (the cut re-tessellation adds cap faces) but not
        // for the shader angle-cut preview, which exposes the raw interior.
        std::optional<bool> dropCoincidentFaces;
        // Opt-in on a merge_descendants group: tag the merged sampling-stack
        // meshes with a StackAverage (area-weighted average color + band
        // width) so the viewer can band-limit the cycling-material moire at
        // distance. Only meaningful together with mergeDescendants (the average
        // is computed over the merged stack); independent of dropCoincidentFaces.
        std::optional<bool> averageMaterialStack;
        std::optional<int> maxSegmentsCircle;
        std::optional<BooleanFallback> fallback;
    };
    std::optional<Tessellation> tessellation;

    std::optional<ExtrasMap> extras; ///< Free-form metadata for export
};

// ── Per-format export overrides ───────────────────────────────────────────────
// In TOML: [export.gltf], [export.glb], [export.obj].
// Each field is optional; absent means "use the format's built-in default".

/// Fields shared by all export formats.
struct CommonExportConfig {
    std::optional<double> unitScale; ///< Overrides the format's default scale
};

struct GltfExportFormatConfig {
    CommonExportConfig common;
    /// Bake the scale into vertices & translations instead of the root node.
    ///
    /// glTF/GLB-only, deliberately: OBJ has no scene graph to carry a root
    /// transform, so `ObjExporter` rejects a false value outright — leaving the
    /// only legal OBJ value `true`, which is already the default. A knob with
    /// one legal value is not a knob, so OBJ does not get this field at all and
    /// `[export.obj] bake_unit_scale` is rejected at load time. That makes the
    /// unsatisfiable combination unrepresentable rather than merely diagnosed:
    /// the Lua builder and any future programmatic constructor get the
    /// guarantee for free, not just the TOML front-end.
    std::optional<bool> bakeUnitScale;
    std::optional<bool> multiScene;                ///< Split render tree into multiple glTF scenes
    std::optional<std::string> sceneNameSeparator; ///< Separator for hierarchical scene names
};

struct ObjExportFormatConfig {
    CommonExportConfig common;
};

using ExportFormatConfig = std::variant<GltfExportFormatConfig, ObjExportFormatConfig>;

/// Visitor to access common config fields from any format variant.
inline const CommonExportConfig &commonConfig(const ExportFormatConfig &cfg) {
    return std::visit([](const auto &c) -> const CommonExportConfig & { return c.common; }, cfg);
}

// ── Top-level config ──────────────────────────────────────────────────────────
// Output path/format are CLI concerns, not config concerns.

struct NHConfig {
    bool hoistOrphans{false};
    bool deduplicateShapes{true}; ///< Merge shapes with identical parameters
    std::map<std::string, ExportFormatConfig> exportFormats; ///< keyed by "gltf", "glb", "obj"
    std::vector<MaterialDef> materials;
    std::vector<SelectionRule> selection;
    std::vector<Rule> rules;

    /// Global fallback for tessellation fields not set by any matching rule.
    Rule::Tessellation tessellationDefaults;
    /// Global fallback extras applied when no rule provides extras.
    std::optional<ExtrasMap> extrasDefaults;
};

} // namespace nodehammer
