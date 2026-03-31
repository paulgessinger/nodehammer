#pragma once

#include <nodehammer/config/config_ast.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/ir/render.hpp>
#include <nodehammer/ir/semantic.hpp>

namespace nodehammer {

struct TessellationPassResult {
    RenderScene scene;
    DiagnosticList diags;
};

/// Lowers a SemanticScene to a RenderScene.
///
/// For each reachable SemanticNode (BFS from root):
///   1. Creates a corresponding RenderNode (preserving hierarchy and transforms).
///   2. Tessellates the node's shape using the first TessellationRule whose scope matches
///      the node's path (or the default rule if none match).
///   3. Assigns material from the first MaterialRule that matches, falling back to a
///      default grey material derived from SourceMaterial.color.
///
/// BooleanShape nodes:
///   - fallback=Skip   → node appears in render scene but has no meshBinding; NH0501 warning.
///   - fallback=BBox   → axis-aligned bounding box proxy mesh; NH0502 warning.
///   - fallback=Fail   → NH0503 error; entire pass returns early with error diagnostics.
///
/// UnknownShape nodes always emit NH0500 error; meshBinding is absent for that node.
class TessellationPass {
  public:
    explicit TessellationPass(const NHConfig &config);

    [[nodiscard]] TessellationPassResult lower(const SemanticScene &scene) const;

  private:
    const NHConfig &config_;
};

} // namespace nodehammer
