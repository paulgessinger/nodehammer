#include <algorithm>
#include <ir/expanded/scene.hpp>
namespace nodehammer::ir::expanded {
void Scene::reseedIdCounters() {
    semantic::GeometryCatalogs::reseedIdCounters();
    for (const auto &[id, node] : nodes) {
        (void)node;
        nextNodeId_ = std::max(nextNodeId_, id.value + 1);
    }
}
void Scene::computeWorldTransforms() {
    if (nodes.empty() || !nodes.contains(rootId)) {
        return;
    }
    nodes.at(rootId).worldTransform = nodes.at(rootId).localTransform;
    visitBFS([this](const Node &node) {
        for (const auto childId : node.children) {
            // Skip rather than throw on a dangling child id, matching visitBFS.
            const auto it = nodes.find(childId);
            if (it == nodes.end()) {
                continue;
            }
            it->second.worldTransform = node.worldTransform * it->second.localTransform;
        }
    });
}

void Scene::computeOriginalPaths() {
    if (nodes.empty() || !nodes.contains(rootId)) {
        return;
    }
    nodes.at(rootId).originalPath = "/" + nodes.at(rootId).name;
    visitBFS([this](const Node &node) {
        for (const auto childId : node.children) {
            // Skip rather than throw on a dangling child id, matching visitBFS.
            const auto it = nodes.find(childId);
            if (it == nodes.end()) {
                continue;
            }
            it->second.originalPath = node.originalPath + "/" + it->second.name;
        }
    });
}

std::size_t Scene::deduplicateLogVols() {
    const auto remap = deduplicateVolumeDefinitions();
    for (auto &[id, node] : nodes) {
        (void)id;
        if (auto it = remap.find(node.logVolId); it != remap.end())
            node.logVolId = it->second;
    }
    return remap.size();
}
} // namespace nodehammer::ir::expanded
