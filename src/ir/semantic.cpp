#include <nodehammer/ir/semantic.hpp>

namespace nodehammer {

void SemanticScene::computeWorldTransforms() {
    if (nodes.empty() || !nodes.contains(rootId)) {
        return;
    }
    nodes.at(rootId).worldTransform = nodes.at(rootId).localTransform;
    visitBFS([this](const SemanticNode &node) {
        for (const auto childId : node.children) {
            auto &child = nodes.at(childId);
            child.worldTransform = node.worldTransform * child.localTransform;
        }
    });
}

} // namespace nodehammer
