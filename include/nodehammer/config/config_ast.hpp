#pragma once

#include <filesystem>
#include <glm/glm.hpp>
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
// Recursive predicates are broken via shared_ptr so PredicateExpr stays copyable.

struct NameGlobPredicate {
    std::string pattern;
};

struct PathGlobPredicate {
    std::string pattern;
};

/// Matches if a node carries the given tag key, and optionally a specific value.
struct TagPredicate {
    std::string key;
    std::optional<std::string> value; ///< nullopt = "key exists", else "key == value"
};

/// True only for nodes that have no children in the scene.
struct IsLeafPredicate {};

// Forward declarations for compound predicates stored via shared_ptr.
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

// ── Material definition ───────────────────────────────────────────────────────

struct MaterialDef {
    std::string name;
    glm::vec4 baseColor{0.8f, 0.8f, 0.8f, 1.0f}; ///< Linear RGBA
    float metallic{0.0f};
    float roughness{0.5f};
    glm::vec3 emissive{0.0f};
    bool doubleSided{false};
};

// ── Rules ─────────────────────────────────────────────────────────────────────

struct SelectionRule {
    SelectionAction action{SelectionAction::KeepIf};
    PredicateExpr predicate{NameGlobPredicate{"*"}};
    ClosurePolicy closure{ClosurePolicy::None};
};

/// Assigns a named MaterialDef to nodes matching a name glob.
struct MaterialRule {
    std::string nameGlob{"*"};
    std::string materialName; ///< Must reference a MaterialDef::name
};

struct TessellationRule {
    std::string nameGlob{"*"};
    int maxSegmentsCircle{64}; ///< Segments for circular cross-sections
    BooleanFallback fallback{BooleanFallback::Skip};
};

// ── Export config ─────────────────────────────────────────────────────────────

struct ExportConfig {
    std::filesystem::path outputPath;
    std::string format; ///< "glb", "gltf", "obj"
    bool embedExtras{false};
};

// ── Top-level config ──────────────────────────────────────────────────────────

struct NHConfig {
    std::vector<MaterialDef> materials;
    std::vector<SelectionRule> selection;
    std::vector<MaterialRule> materialRules;
    std::vector<TessellationRule> tessellationRules;
    ExportConfig exportCfg;
};

} // namespace nodehammer
