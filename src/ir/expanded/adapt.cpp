#include <ir/expanded/adapt.hpp>
#include <unordered_set>

namespace nodehammer::ir::semantic {
Scene fromExpanded(const expanded::Scene &scene) {
    using expanded::Node;
    using expanded::NodeId;
    if (scene.nodes.empty() && scene.logVols.empty() && scene.rootId.value == 0) {
        Scene result;
        static_cast<GeometryCatalogs &>(result) = scene;
        return result;
    }
    if (!scene.nodes.contains(scene.rootId) || scene.nodes.at(scene.rootId).parentId) {
        throw std::invalid_argument("expanded scene requires a valid parentless root");
    }
    Scene result;
    result.sourceFile = scene.sourceFile;
    result.shapes = scene.shapes;
    result.materials = scene.materials;
    const auto &root = scene.nodes.at(scene.rootId);
    result.root = {root.name, LogVolId{root.id.value}, root.localTransform};
    std::map<OccurrenceTags, uint64_t> interned;
    struct Pending {
        NodeId node;
        OccurrenceId path;
    };
    std::vector<Pending> pending{{scene.rootId, {}}};
    std::unordered_set<NodeId> visited;
    while (!pending.empty()) {
        auto current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current.node).second) {
            throw std::invalid_argument("expanded scene has a cycle or repeated child");
        }
        const auto &node = scene.nodes.at(current.node);
        if (node.id != current.node) {
            throw std::invalid_argument("expanded node key/id mismatch");
        }
        const auto &sourceVolume = scene.logVols.at(node.logVolId);
        if (sourceVolume.id != node.logVolId) {
            throw std::invalid_argument("expanded volume key/id mismatch");
        }
        LogicalVolume volume{LogVolId{node.id.value}, sourceVolume.name, sourceVolume.shapeId,
                             sourceVolume.materialId};
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            const auto &child = scene.nodes.at(node.children[i]);
            if (child.parentId != node.id) {
                throw std::invalid_argument("expanded parent/child links disagree");
            }
            volume.daughters.push_back(
                {child.name, LogVolId{child.id.value}, child.localTransform});
            auto path = current.path;
            path.push_back(i);
            pending.push_back({child.id, std::move(path)});
        }
        result.logVols.emplace(volume.id, std::move(volume));
        if (!node.tags.empty()) {
            auto [it, inserted] = interned.emplace(node.tags, result.metadata.tagSets.size());
            if (inserted)
                result.metadata.tagSets.push_back(node.tags);
            result.metadata.volumeTags.emplace(LogVolId{node.id.value}, it->second);
        }
        result.metadata.originalPaths.emplace(current.path, node.originalPath);
        if (!node.sourceSystem.empty())
            result.metadata.sourceSystems.emplace(current.path, node.sourceSystem);
        if (node.degradation.any())
            result.metadata.degradation.emplace(current.path, node.degradation);
    }
    if (visited.size() != scene.nodes.size()) {
        throw std::invalid_argument("expanded scene has disconnected nodes");
    }
    (void)result.validate();
    return result;
}
} // namespace nodehammer::ir::semantic
