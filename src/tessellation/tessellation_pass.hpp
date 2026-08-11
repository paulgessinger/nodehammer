#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/render.hpp>
#include <ir/semantic.hpp>

namespace nodehammer::tessellation {

struct TessellationPassResult {
    ir::render::Scene scene;
    DiagnosticList diags;
};

/// Lowers a semantic::Scene to a render::Scene.
///
/// For each reachable semantic::Node (BFS from root):
///   1. Creates a corresponding render::Node (preserving hierarchy and transforms).
///   2. Tessellates the node's shape using the first matching Rule with tessellation settings
///      (or defaults if none match).
///   3. Assigns material from the first matching Rule with a material name, falling back to a
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
    explicit TessellationPass(const config::NHConfig &config);

    [[nodiscard]] TessellationPassResult lower(const ir::semantic::Scene &scene) const;

  private:
    const config::NHConfig &config_;
};

} // namespace nodehammer::tessellation
