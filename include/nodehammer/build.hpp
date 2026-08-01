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

// All four follow one contract, and it is the internal pipeline's: the result
// carries whatever the stage produced *and* everything the stage had to say,
// and the diagnostics are what tell you whether to use it. A tessellation that
// meets an unknown shape reports NH0500 and returns a scene with that one node
// unmeshed; discarding the rest would be this layer overruling a decision the
// pass made deliberately. Always check `result.diags.hasErrors()` — whether the
// handle is valid is a different question, and a weaker one.
//
// `build` carries the one unavoidable exception; see its declaration.

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
///
/// The one place in this API where an error really does mean no result: a
/// failed selection has produced no render scene *yet*, so there is nothing to
/// return but the reason. Tessellation errors, by contrast, come back with the
/// scene, as they do from `tessellate`.
[[nodiscard]] NH_API RenderResult build(const SemanticScene &scene, const SceneConfig &config);

} // namespace nodehammer
