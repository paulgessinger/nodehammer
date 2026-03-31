#include <nodehammer/selection/closure.hpp>

#include <queue>
#include <stdexcept>

namespace nodehammer {

namespace {

void addAncestors(const SemanticScene &scene, const std::unordered_set<SemanticNodeId> &seed,
                  std::unordered_set<SemanticNodeId> &result) {
    for (const auto &startId : seed) {
        if (!scene.nodes.contains(startId)) {
            throw std::invalid_argument("ClosureExpander: seed ID not present in scene");
        }
        auto current = scene.nodes.at(startId).parentId;
        while (current.has_value()) {
            if (!scene.nodes.contains(*current)) {
                break;
            }
            if (!result.insert(*current).second) {
                // Already present; its ancestors were already added on a prior traversal.
                break;
            }
            current = scene.nodes.at(*current).parentId;
        }
    }
}

void addDescendants(const SemanticScene &scene, const std::unordered_set<SemanticNodeId> &seed,
                    std::unordered_set<SemanticNodeId> &result) {
    std::queue<SemanticNodeId> q;
    for (const auto &id : seed) {
        if (!scene.nodes.contains(id)) {
            throw std::invalid_argument("ClosureExpander: seed ID not present in scene");
        }
        q.push(id);
    }
    while (!q.empty()) {
        const auto current = q.front();
        q.pop();
        for (const auto childId : scene.nodes.at(current).children) {
            if (!scene.nodes.contains(childId)) {
                continue; // dangling internal reference — malformed scene, skip defensively
            }
            if (result.insert(childId).second) {
                q.push(childId);
            }
        }
    }
}

} // namespace

std::unordered_set<SemanticNodeId>
ClosureExpander::expand(const SemanticScene &scene, const std::unordered_set<SemanticNodeId> &seed,
                        ClosurePolicy policy) {
    std::unordered_set<SemanticNodeId> result = seed;

    switch (policy) {
    case ClosurePolicy::None:
        break;
    case ClosurePolicy::Ancestors:
        addAncestors(scene, seed, result);
        break;
    case ClosurePolicy::Descendants:
        addDescendants(scene, seed, result);
        break;
    case ClosurePolicy::Full:
        addAncestors(scene, seed, result);
        addDescendants(scene, seed, result);
        break;
    }

    return result;
}

} // namespace nodehammer
