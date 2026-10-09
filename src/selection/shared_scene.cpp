#include <ir/expanded/conversion.hpp>
#include <selection/occurrence_selector.hpp>
#include <selection/selector.hpp>

namespace nodehammer::ir::semantic {
DiagnosticList select(Scene &scene, const std::vector<config::SelectionRule> &rules, bool hoist) {
    if (scene.nodeCount() == 0)
        return {};
    auto tree = scene.makeTree(0);
    const selection::SelectedOccurrences original{*tree};
    const auto selected = original.prune(rules, hoist);
    auto diags = selected.diagnostics();
    if (diags.hasErrors())
        return diags;
    if (selected.size() == original.size()) {
        scene.collectGarbage();
        return diags;
    }
    // Project retained occurrences directly into source definitions. No physical
    // Nodes or world-transform cache is constructed before the rendering boundary.
    Scene result;
    result.sourceFile = scene.sourceFile;
    result.shapes = scene.shapes;
    result.materials = scene.materials;
    struct Output {
        LogVolId volume;
        OccurrenceId path;
    };
    std::map<OccurrenceId, Output> outputs;
    std::map<OccurrenceTags, uint64_t> tagSets;
    struct Frame {
        OccurrenceId path;
        LogVolId volume;
        std::size_t next{};
        bool entered{};
    };
    std::vector<Frame> stack{{{}, scene.root.logVolId}};
    while (!stack.empty()) {
        auto &frame = stack.back();
        if (!frame.entered) {
            if (selected.subtreeSize(frame.path) == 0) {
                stack.pop_back();
                continue;
            }
            frame.entered = true;
            if (selected.contains(frame.path)) {
                const auto item = selected.materialize(frame.path);
                const auto &source = *item.source;
                const auto &prototype = scene.logVols.at(source.logVolId);
                const auto id = result.nextLogVolId();
                result.logVols.emplace(
                    id, LogicalVolume{id, prototype.name, prototype.shapeId, prototype.materialId});
                DaughterPlacement placement{source.name, id, item.localTransform};
                OccurrenceId outputPath;
                if (item.parentId) {
                    const auto &parent = outputs.at(*item.parentId);
                    auto &daughters = result.logVols.at(parent.volume).daughters;
                    outputPath = parent.path;
                    outputPath.push_back(daughters.size());
                    daughters.push_back(std::move(placement));
                } else
                    result.root = std::move(placement);
                outputs.emplace(frame.path, Output{id, outputPath});
                result.metadata.originalPaths.emplace(outputPath, source.originalPath);
                if (!source.tags.empty()) {
                    auto [it, added] = tagSets.emplace(source.tags, result.metadata.tagSets.size());
                    if (added)
                        result.metadata.tagSets.push_back(source.tags);
                    result.metadata.occurrenceTags.emplace(outputPath, it->second);
                }
                if (!source.sourceSystem.empty())
                    result.metadata.sourceSystems.emplace(outputPath, source.sourceSystem);
                if (source.degradation.any())
                    result.metadata.degradation.emplace(outputPath, source.degradation);
            }
        }
        const auto &daughters = scene.logVols.at(frame.volume).daughters;
        if (frame.next == daughters.size()) {
            stack.pop_back();
            continue;
        }
        auto path = frame.path;
        const auto index = frame.next++;
        path.push_back(index);
        stack.push_back({std::move(path), daughters[index].logVolId});
    }
    result.collectGarbage();
    scene = std::move(result);
    return diags;
}
} // namespace nodehammer::ir::semantic

namespace nodehammer::selection {
DiagnosticList SelectionEngine::prune(ir::semantic::Scene &scene) const {
    return ir::semantic::select(scene, rules_, hoistOrphans_);
}
} // namespace nodehammer::selection
