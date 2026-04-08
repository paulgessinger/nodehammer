#pragma once

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

/// Assigns a named material to nodes in an optional scope, with an optional additional predicate.
/// In TOML: material = "name"; scope and match are optional.
/// Future: match may be a string expression (match = "tag.semantic == sensor") once a DSL parser
/// is added; the structured table form will remain valid.
struct MaterialRule {
    std::optional<std::string> scope;   ///< Optional path glob pre-filter
    std::optional<PredicateExpr> match; ///< Optional additional predicate within scope
    std::string materialName;           ///< References a MaterialDef::name
};

struct TessellationRule {
    std::optional<std::string> scope; ///< Optional path glob (matches all if absent)
    bool skipGeometry{false};         ///< If true, node is kept in tree but produces no mesh
    bool mergeChildren{false}; ///< If true, merge all descendant meshes into one on this node
    int maxSegmentsCircle{64};
    BooleanFallback fallback{BooleanFallback::Skip};
};

// ── Per-format export overrides ───────────────────────────────────────────────
// In TOML: [export.gltf], [export.glb], [export.obj].
// Each field is optional; absent means "use the format's built-in default".

struct ExportFormatConfig {
    std::optional<double> unitScale;   ///< Overrides the format's default scale
    std::optional<bool> bakeUnitScale; ///< Bake scale into vertices & translations
};

// ── Top-level config ──────────────────────────────────────────────────────────
// Output path/format are CLI concerns, not config concerns.

struct NHConfig {
    bool hoistOrphans{false};
    std::map<std::string, ExportFormatConfig> exportFormats; ///< keyed by "gltf", "glb", "obj"
    std::vector<MaterialDef> materials;
    std::vector<SelectionRule> selection;
    std::vector<MaterialRule> materialRules;
    std::vector<TessellationRule> tessellationRules;
};

} // namespace nodehammer
