#include <nodehammer/ir/semantic.hpp>
#include <queue>

namespace nodehammer {

void SemanticScene::computeWorldTransforms() {
    if (nodes.empty())
        return;

    auto it = nodes.find(rootId);
    if (it == nodes.end())
        return;

    // BFS from root: compose parent worldTransform × child localTransform
    std::queue<SemanticNodeId> queue;
    it->second.worldTransform = it->second.localTransform;
    queue.push(rootId);

    while (!queue.empty()) {
        const SemanticNodeId parentId = queue.front();
        queue.pop();

        const auto &parent = nodes.at(parentId);
        for (const SemanticNodeId childId : parent.children) {
            auto &child = nodes.at(childId);
            child.worldTransform = parent.worldTransform * child.localTransform;
            queue.push(childId);
        }
    }
}

} // namespace nodehammer
