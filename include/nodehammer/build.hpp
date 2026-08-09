#pragma once

// The pipeline verbs.
//
// Free functions rather than members because each one spans two types and
// belongs to neither, and because making `tessellate` a member of SemanticScene
// would put SceneConfig and RenderScene into the connector header (#41 §4).

#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/visibility.hpp>

namespace nodehammer {

// All four throw `Error` if handed a scene handle that refers to nothing —
// that is a caller mistake, with no result to attach a reason to.
//
// Everything the *work* has to say comes back in `diags`, at any severity. A
// tessellation that meets an unknown shape reports NH0500 and returns a scene
// with that one node unmeshed; discarding the rest would be this layer
// overruling a decision the pass made deliberately, so the caller decides
// whether that scene is worth exporting. Check `result.diags.hasErrors()`.

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
/// read, and a config that does not validate never reaches here (#41 §8).
///
/// Throws if selection fails — NH0401, the root itself dropped. That stage
/// runs before tessellation, so no render scene exists to hand back with the
/// reason attached, and tessellating a scene the rules meant to prune would be
/// worse than stopping. `convert` stops there too. Tessellation's own errors,
/// by contrast, come back with the scene, exactly as from `tessellate`.
[[nodiscard]] NH_API RenderResult build(const SemanticScene &scene, const SceneConfig &config);

} // namespace nodehammer
