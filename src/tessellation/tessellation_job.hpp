#pragma once

#include <tessellation/tessellation_pass.hpp>

#include <cstdint>
#include <memory>

namespace nodehammer::tessellation {

/// Cooperative, iterator-driven version of `TessellationPass::lower`. The
/// pass walks every reachable `semantic::Node` once; this class lets the
/// caller spread that walk across multiple `advance()` calls so the main
/// thread (web build) stays responsive while a large scene tessellates.
///
/// Usage: `start(config, scene)`, then call `advance(budget_ns)` from the
/// frame loop until it returns true, then `take()` the result.
///
/// `config` and `scene` must outlive the job until `advance` returns true.
class TessellationJob {
  public:
    TessellationJob();
    ~TessellationJob();
    TessellationJob(const TessellationJob &) = delete;
    TessellationJob &operator=(const TessellationJob &) = delete;
    TessellationJob(TessellationJob &&) noexcept;
    TessellationJob &operator=(TessellationJob &&) noexcept;

    /// Initialise the job. Compiles rule predicates and seeds the BFS
    /// queue with the scene root.
    void start(const config::NHConfig &config, const ir::semantic::Scene &scene);

    /// Process pending semantic nodes for up to `budget_ns` of wall-clock
    /// time. The atomic unit is one outer-BFS iteration (one semantic
    /// node, plus any merge_descendants subtree it consumes); a node
    /// whose tessellation cost exceeds the budget will still complete in
    /// a single advance call rather than splitting mid-node.
    /// Returns true once the queue drains and the result is ready.
    bool advance(uint64_t budget_ns = 8'000'000);

    /// Move out the accumulated result. Valid only after `advance()`
    /// returns true.
    [[nodiscard]] TessellationPassResult take();

    /// Progress reporting for UI feedback. `total` is the count of
    /// semantic nodes reachable from the root (set after `start`);
    /// `processed` grows as `advance` runs.
    [[nodiscard]] size_t totalNodes() const;
    [[nodiscard]] size_t processedNodes() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nodehammer::tessellation
