#include <diagnostic_codes.hpp>
#include <ir/expanded/adapt.hpp>
#include <ir/expanded/conversion.hpp>
#include <ir/semantic/occurrence_tree.hpp>

namespace nodehammer::ir::semantic {
using expanded::Node;
using expanded::NodeId;

void visit(const Scene &scene, const std::function<bool(const Occurrence &, bool)> &fn) {
    if (scene.nodeCount() == 0)
        return;
    auto tree = scene.makeTree(0);
    struct Frame {
        OccurrenceId path;
        std::size_t next{};
        std::size_t count{};
    };
    const auto root = tree->materialize({});
    if (!fn(*root, true))
        return;
    std::vector<Frame> stack{{{}, 0, root->childCount}};
    while (!stack.empty()) {
        auto &frame = stack.back();
        if (frame.next == frame.count) {
            stack.pop_back();
            continue;
        }
        auto path = frame.path;
        path.push_back(frame.next++);
        const bool last = frame.next == frame.count;
        const auto node = tree->materialize(path);
        if (fn(*node, last))
            stack.push_back({std::move(path), 0, node->childCount});
    }
}
expanded::Scene expand(const Scene &scene) {
    expanded::Scene result{static_cast<const GeometryCatalogs &>(scene)};
    std::map<OccurrenceId, NodeId> ids;
    visit(scene, [&](const auto &source, bool) {
        Node node;
        node.id = result.nextNodeId();
        node.name = source.name;
        node.logVolId = source.logVolId;
        node.localTransform = source.localTransform;
        node.worldTransform = source.worldTransform;
        node.originalPath = source.originalPath;
        node.tags = source.tags;
        node.sourceSystem = source.sourceSystem;
        node.degradation = source.degradation;
        if (source.id.empty())
            result.rootId = node.id;
        else {
            auto parent = source.id;
            parent.pop_back();
            node.parentId = ids.at(parent);
            result.nodes.at(*node.parentId).children.push_back(node.id);
        }
        ids.emplace(source.id, node.id);
        result.nodes.emplace(node.id, std::move(node));
        return true;
    });
    result.reseedIdCounters();
    return result;
}
} // namespace nodehammer::ir::semantic
