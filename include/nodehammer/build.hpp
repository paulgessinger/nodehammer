#pragma once

// The pipeline verbs.
//
// Free functions rather than members because each one spans two types and
// belongs to neither, and because making `tessellate` a member of SemanticScene
// would put SceneConfig and RenderScene into the connector header (#41 §4).

#include <nodehammer/api.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>

namespace nodehammer {

// All four follow one contract: warnings ride along with a valid result, and an
// *error* means the returned handle is invalid — the stage that failed produced
// nothing usable, which is the same call the CLI makes when it stops rather
// than exporting a scene it knows to be wrong. Always check
// `result.diags.hasErrors()` before using the handle.

/// Evaluate `[[selection_rules]]` and prune what they drop, garbage-collecting
/// the logical volumes, shapes and materials that nothing references any more.
///
/// A config with no selection rules is a no-op, matching the CLI: "no rules"
/// means no filtering, not "keep everything and rebuild".
[[nodiscard]] NH_API SemanticResult applySelection(const SemanticScene &scene,
                                                   const SceneConfig &config);

/// Merge materials, shapes and logical volumes that are equal by value.
///
/// Its own verb, and load-bearing: without it `applySelection` + `tessellate`
/// would silently differ from `build` and from the CLI *even with*
/// `deduplicate_shapes = true`, with no diagnostic to say so (#41 §8). Honours
/// that flag — a config that turns dedup off makes this a no-op.
[[nodiscard]] NH_API SemanticResult deduplicate(const SemanticScene &scene,
                                                const SceneConfig &config);

/// Lower a semantic scene to triangles. One pass, nothing else: no selection,
/// no deduplication.
[[nodiscard]] NH_API RenderResult tessellate(const SemanticScene &scene, const SceneConfig &config);

/// `applySelection` then `deduplicate` then `tessellate`, in that order — the
/// same order, and the same conditions, as `nodehammer convert`.
///
/// Config *validation* is not part of this: it happens when the document is
/// read and surfaces in that call's diagnostics (#41 §8).
[[nodiscard]] NH_API RenderResult build(const SemanticScene &scene, const SceneConfig &config);

} // namespace nodehammer
