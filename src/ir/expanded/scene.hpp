#pragma once
#include <ir/semantic.hpp>
namespace nodehammer::ir::expanded {
using namespace semantic;
struct NodeTag {};
using NodeId = StrongId<NodeTag>;
// ── Node ──────────────────────────────────────────────────────────────────────

struct Node {
    NodeId id;
    std::string name;
    LogVolId logVolId;

    glm::dmat4 localTransform{1.0}; ///< Relative to parent
    glm::dmat4 worldTransform{1.0}; ///< Set by computeWorldTransforms()

    std::optional<NodeId> parentId;
    std::vector<NodeId> children;

    /// Full path in the original source tree, e.g. "/world/ODD/PixelBarrel/sensor_0".
    /// Set by computeOriginalPaths() before selection; preserved across hoisting.
    std::string originalPath;

    /// Free-form metadata tags (e.g. "subdetector"="tracker", "sensitive"="true")
    std::map<std::string, std::string> tags;

    std::string sourceSystem; ///< e.g. "dd4hep", "dd4hep/tgeo", "tgeo"
    DegradationFlags degradation;
};

// ── Scene ─────────────────────────────────────────────────────────────────────

class Scene : public semantic::GeometryCatalogs {
  public:
    Scene() = default;
    explicit Scene(semantic::GeometryCatalogs catalogs)
        : semantic::GeometryCatalogs(std::move(catalogs)) {
        reseedIdCounters();
    }
    [[nodiscard]] uint64_t nodeCount() const noexcept { return nodes.size(); }

    NodeId rootId;

    // Flat maps indexed by ID
    ankerl::unordered_dense::map<NodeId, Node> nodes;

    /// Reseed the ID allocation counters so that nextXxxId() returns values
    /// greater than every ID currently present in the maps. Deserializing
    /// importers (JSON/FlatBuffer) populate the maps directly with pre-existing
    /// IDs without advancing the counters; call this before allocating new IDs
    /// on a loaded scene to avoid colliding with — and overwriting — them.
    void reseedIdCounters();

    /// BFS pass: compose parent × local to set worldTransform on every node.
    void computeWorldTransforms();

    /// BFS pass: build originalPath from root for every reachable node.
    void computeOriginalPaths();

    /// Deduplicate logical volumes: logVols with identical (shapeId, materialId)
    /// are merged, and all referencing nodes are updated.
    /// Returns the number of logical volumes removed.
    std::size_t deduplicateLogVols();

    /// BFS traversal from root; calls fn(const expanded::Node &) for every reachable node.
    /// Guards on both a missing id and a repeat visit, so a dangling child id is
    /// skipped rather than throwing from inside the traversal and a cycle
    /// terminates rather than looping forever. Matches the guards in
    /// `reachableNodes` (src/selection/selector.cpp).
    template <typename Fn> void visitBFS(Fn &&fn) const {
        if (nodes.empty() || !nodes.contains(rootId)) {
            return;
        }
        std::unordered_set<NodeId> seen;
        seen.reserve(nodes.size());
        std::queue<NodeId> q;
        q.push(rootId);
        while (!q.empty()) {
            const auto id = q.front();
            q.pop();
            const auto it = nodes.find(id);
            if (it == nodes.end() || !seen.insert(id).second) {
                continue;
            }
            fn(it->second);
            for (const auto childId : it->second.children) {
                q.push(childId);
            }
        }
    }

    // ID allocation
    NodeId nextNodeId() { return NodeId{nextNodeId_++}; }

  private:
    uint64_t nextNodeId_{1};
};

struct ImportResult {
    Scene scene;
    DiagnosticList diags;
};
} // namespace nodehammer::ir::expanded
