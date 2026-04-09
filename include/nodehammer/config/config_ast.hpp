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

enum class ClosurePolicy { None, Ancestors, Descendants, Full };
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

/// True when a node carries the given tag key, and optionally a specific value.
/// value = nullopt means "key exists"; value = "x" means "key == 'x'".
struct TagPredicate {
    std::string key;
    std::optional<std::string> value;
};

/// True only for nodes that have no children in the scene.
struct IsLeafPredicate {};

struct AndPredicate;
struct OrPredicate;
struct NotPredicate;

using PredicateVariant = std::variant<NameGlobPredicate, PathGlobPredicate, TagPredicate,
                                      IsLeafPredicate, std::shared_ptr<AndPredicate>,
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

struct MaterialDef {
    std::string name; ///< Populated from the TOML table key
    Color baseColor{0.8f, 0.8f, 0.8f, 1.0f};
    float metallic{0.0f};
    float roughness{0.5f};
    Color emissive{0.0f, 0.0f, 0.0f, 1.0f};
    bool doubleSided{false};
};

// ── Rules ─────────────────────────────────────────────────────────────────────
// In TOML: [[selection_rules]] with keys keep_if or drop_if (not action = "...").
// scope is an optional path glob that restricts the rule to a subtree.

struct SelectionRule {
    SelectionAction action{SelectionAction::KeepIf};
    std::optional<std::string> scope; ///< Optional path glob pre-filter
    PredicateExpr predicate{NameGlobPredicate{"*"}};
    ClosurePolicy closure{ClosurePolicy::None};
};

/// Free-form extras for export metadata (e.g. glTF extras).
/// Uses nlohmann::json as a recursive value type — maps directly to tinygltf::Value.
using ExtrasMap = nlohmann::json;

/// Unified rule: optional material, tessellation, and/or extras on matched nodes.
/// In TOML: [[rules]] with scope, optional match, and sub-tables.
struct Rule {
    std::optional<std::string> scope;   ///< Optional path glob pre-filter
    std::optional<PredicateExpr> match; ///< Optional additional predicate within scope

    // ── Concerns (all optional; a rule may set any combination) ──────────
    std::optional<std::string> material; ///< References a MaterialDef::name

    struct Tessellation {
        bool skipGeometry{false};
        bool mergeChildren{false};
        int maxSegmentsCircle{64};
        BooleanFallback fallback{BooleanFallback::Skip};
    };
    std::optional<Tessellation> tessellation;

    std::optional<ExtrasMap> extras; ///< Free-form metadata for export
};

// ── Per-format export overrides ───────────────────────────────────────────────
// In TOML: [export.gltf], [export.glb], [export.obj].
// Each field is optional; absent means "use the format's built-in default".

/// Fields shared by all export formats.
struct CommonExportConfig {
    std::optional<double> unitScale;   ///< Overrides the format's default scale
    std::optional<bool> bakeUnitScale; ///< Bake scale into vertices & translations
};

struct GltfExportFormatConfig {
    CommonExportConfig common;
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
    std::map<std::string, ExportFormatConfig> exportFormats; ///< keyed by "gltf", "glb", "obj"
    std::vector<MaterialDef> materials;
    std::vector<SelectionRule> selection;
    std::vector<Rule> rules;
};

} // namespace nodehammer
