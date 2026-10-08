#include <selection/occurrence_selector.hpp>

#include <diagnostic_codes.hpp>
#include <selection/predicate.hpp>

#include <glm/gtc/matrix_inverse.hpp>

#include <bit>
#include <format>
#include <limits>
#include <unordered_map>

namespace nodehammer::selection {
namespace detail {
using namespace ir::semantic;

struct OccurrenceMask {
    std::vector<uint64_t> words;
    std::vector<uint64_t> prefix;
    explicit OccurrenceMask(uint64_t count) {
        const auto wordCount = count / 64 + (count % 64 != 0 ? 1 : 0);
        if (wordCount >= words.max_size() || wordCount >= prefix.max_size()) {
            throw std::length_error("occurrence selection mask is too large");
        }
        words.resize(static_cast<std::size_t>(wordCount));
        prefix.resize(static_cast<std::size_t>(wordCount) + 1);
    }
    void set(uint64_t ordinal) {
        words.at(static_cast<std::size_t>(ordinal / 64)) |= uint64_t{1} << (ordinal % 64);
    }
    void finish() {
        for (std::size_t i = 0; i < words.size(); ++i) {
            prefix.at(i + 1) = prefix.at(i) + static_cast<uint64_t>(std::popcount(words.at(i)));
        }
    }
    [[nodiscard]] bool contains(uint64_t ordinal) const {
        return (words.at(static_cast<std::size_t>(ordinal / 64)) &
                (uint64_t{1} << (ordinal % 64))) != 0;
    }
    [[nodiscard]] uint64_t rank(uint64_t end) const {
        const auto word = static_cast<std::size_t>(end / 64);
        const auto bits = end % 64;
        return prefix.at(word) + (bits == 0 ? 0
                                            : static_cast<uint64_t>(std::popcount(
                                                  words.at(word) & ((uint64_t{1} << bits) - 1))));
    }
    [[nodiscard]] bool any(uint64_t begin, uint64_t end) const { return rank(end) != rank(begin); }
    [[nodiscard]] uint64_t size() const { return prefix.back(); }
};

struct OccurrenceIndex {
    OccurrenceTree &tree;
    std::unordered_map<LogVolId, uint64_t> counts;
    std::unordered_map<LogVolId, std::vector<uint64_t>> childOffsets;
    uint64_t total{};

    explicit OccurrenceIndex(OccurrenceTree &input) : tree(input) {
        // Memoized postorder over prototypes: no physical tree expansion, no
        // recursive C++ call stack, and cycles/overflow are rejected before use.
        struct Frame {
            LogVolId id;
            std::size_t next{};
            uint64_t count{1};
        };
        const auto &volumes = tree.sourceScene().logVols;
        std::vector<Frame> stack{{tree.rootPlacement().logVolId}};
        std::unordered_set<LogVolId> active{stack.back().id};
        while (!stack.empty()) {
            auto &frame = stack.back();
            const auto &daughters = volumes.at(frame.id).daughters;
            if (frame.next == daughters.size()) {
                counts.emplace(frame.id, frame.count);
                std::vector<uint64_t> offsets;
                offsets.reserve(daughters.size());
                uint64_t offset = 1;
                for (const auto &daughter : daughters) {
                    offsets.push_back(offset);
                    offset += counts.at(daughter.logVolId);
                }
                childOffsets.emplace(frame.id, std::move(offsets));
                active.erase(frame.id);
                stack.pop_back();
                continue;
            }
            const auto child = daughters.at(frame.next).logVolId;
            auto it = counts.find(child);
            if (it == counts.end()) {
                if (!active.insert(child).second) {
                    throw std::invalid_argument("cycle in occurrence prototype ancestry");
                }
                stack.push_back({child});
                continue;
            }
            if (it->second > std::numeric_limits<uint64_t>::max() - frame.count) {
                throw std::overflow_error("occurrence count exceeds uint64_t");
            }
            frame.count += it->second;
            ++frame.next;
        }
        total = counts.at(tree.rootPlacement().logVolId);
    }

    [[nodiscard]] std::pair<uint64_t, uint64_t> interval(const OccurrenceId &id) const {
        auto volume = tree.rootPlacement().logVolId;
        uint64_t ordinal = 0;
        for (const auto index : id) {
            const auto &daughters = tree.sourceScene().logVols.at(volume).daughters;
            const auto &daughter = daughters.at(index); // Validate before summing siblings.
            ordinal += childOffsets.at(volume).at(index);
            volume = daughter.logVolId;
        }
        return {ordinal, ordinal + counts.at(volume)};
    }

    // Predicate traversal builds one reusable path, with O(depth) stack/identity.
    // Absent intermediate ancestors are traversed only to reach hoisted survivors.
    // Visitor returns this node's effective drop disposition for the next level.
    template <class Visitor> void walk(const OccurrenceMask *present, Visitor &&visitor) const {
        struct Frame {
            LogVolId volume;
            std::string_view name;
            uint64_t ordinal;
            uint64_t end;
            uint64_t nextOrdinal;
            std::size_t nextChild{};
            std::size_t parentPathSize{};
            bool entered{};
            bool parentDropped{};
            std::string_view parentName;
        };
        const auto &root = tree.rootPlacement();
        const auto &source = tree.sourceScene();
        std::vector<Frame> stack{{root.logVolId, root.name, 0, total, 1, 0, 0, false, false, {}}};
        OccurrenceId id;
        std::string path = "/" + std::string{tree.pathNameFor({}, root.name)};
        while (!stack.empty()) {
            auto &frame = stack.back();
            const auto &volume = source.logVols.at(frame.volume);
            if (!frame.entered) {
                frame.entered = true;
                if (!present || present->contains(frame.ordinal)) {
                    std::string_view material;
                    if (auto it = source.materials.find(volume.materialId);
                        it != source.materials.end()) {
                        material = it->second.name;
                    }
                    const bool leaf = present ? !present->any(frame.ordinal + 1, frame.end)
                                              : volume.daughters.empty();
                    const auto placement =
                        stack.size() > 1
                            ? std::optional{std::pair{stack.at(stack.size() - 2).volume, id.back()}}
                            : std::nullopt;
                    const NodeView view{tree.displayNameFor(id, frame.name),
                                        tree.originalPathFor(id, path), material, leaf,
                                        &tree.tagsFor(id, frame.volume, placement)};
                    frame.parentDropped =
                        visitor(frame.ordinal, view, frame.parentDropped, frame.parentName);
                    frame.parentName = view.name;
                }
            }
            if (frame.nextChild == volume.daughters.size()) {
                const auto pathSize = frame.parentPathSize;
                stack.pop_back();
                if (!id.empty()) {
                    id.pop_back();
                }
                path.resize(pathSize);
                continue;
            }
            const auto childIndex = frame.nextChild++;
            const auto &daughter = volume.daughters.at(childIndex);
            const auto ordinal = frame.nextOrdinal;
            const auto end = ordinal + counts.at(daughter.logVolId);
            frame.nextOrdinal = end;
            if (present && !present->any(ordinal, end)) {
                continue;
            }
            const auto pathSize = path.size();
            path += "/";
            id.push_back(childIndex);
            path += tree.pathNameFor(id, daughter.name);
            stack.push_back({daughter.logVolId, daughter.name, ordinal, end, ordinal + 1, 0,
                             pathSize, false, frame.parentDropped, frame.parentName});
        }
    }
};
} // namespace detail

uint64_t OccurrenceEvaluation::keptCount() const { return mask_->size(); }
uint64_t OccurrenceEvaluation::droppedCount() const { return inputCount_ - keptCount(); }
bool OccurrenceEvaluation::contains(const OccurrenceId &id) const {
    return mask_->contains(index_->interval(id).first);
}

SelectedOccurrences::SelectedOccurrences(OccurrenceTree &tree, OccurrenceSelectionOptions options)
    : index_(std::make_shared<detail::OccurrenceIndex>(tree)), options_(options) {}

SelectedOccurrences::SelectedOccurrences(std::shared_ptr<const detail::OccurrenceIndex> index,
                                         std::shared_ptr<const detail::OccurrenceMask> mask,
                                         DiagnosticList diags, OccurrenceSelectionOptions options,
                                         uint64_t orphanWarningCount,
                                         uint64_t omittedOrphanDiagnosticCount)
    : index_(std::move(index)), mask_(std::move(mask)), options_(options), diags_(std::move(diags)),
      orphanWarningCount_(orphanWarningCount),
      omittedOrphanDiagnosticCount_(omittedOrphanDiagnosticCount) {}

uint64_t SelectedOccurrences::size() const { return mask_ ? mask_->size() : index_->total; }
uint64_t SelectedOccurrences::subtreeSize(const OccurrenceId &id) const {
    const auto [begin, end] = index_->interval(id);
    return mask_ ? mask_->rank(end) - mask_->rank(begin) : end - begin;
}
bool SelectedOccurrences::contains(const OccurrenceId &id) const {
    const auto ordinal = index_->interval(id).first;
    return !mask_ || mask_->contains(ordinal);
}
std::size_t SelectedOccurrences::storageBytes() const {
    return mask_ ? (mask_->words.size() + mask_->prefix.size()) * sizeof(uint64_t) : 0;
}

OccurrenceEvaluation SelectedOccurrences::dryRun(const std::vector<config::SelectionRule> &rules,
                                                 bool hoist) const {
    struct Rule {
        config::SelectionAction action;
        Predicate predicate;
        std::optional<Predicate> scope;
    };
    std::vector<Rule> compiled;
    for (const auto &rule : rules) {
        compiled.push_back(
            {rule.action, compilePredicate(rule.predicate),
             rule.scope ? std::optional{makePathGlobPredicate(*rule.scope)} : std::nullopt});
    }
    OccurrenceEvaluation result;
    result.index_ = index_;
    result.inputCount_ = size();
    result.mask_ = std::make_shared<detail::OccurrenceMask>(index_->total);
    index_->walk(mask_.get(), [&](uint64_t ordinal, const NodeView &view, bool parentDropped,
                                  std::string_view parentName) {
        auto action = config::SelectionAction::KeepIf;
        for (const auto &rule : compiled) {
            if ((!rule.scope || (*rule.scope)(view)) && rule.predicate(view)) {
                action = rule.action;
            }
        }
        bool drop = action == config::SelectionAction::DropIf;
        if (!hoist && parentDropped && !drop) {
            // Each present occurrence is visited once and the root cannot be
            // an orphan, so both counters remain below the checked source count.
            ++result.orphanWarningCount_;
            if (!options_.orphanDiagnosticLimit ||
                result.orphanWarningCount_ <= *options_.orphanDiagnosticLimit) {
                result.diags_.warn(
                    codes::kWarnSelectionOrphan,
                    std::format("node '{}' kept but parent '{}' dropped -- forcing drop", view.name,
                                parentName),
                    std::string{view.name});
            } else {
                // Do not allocate or format a per-occurrence message after the
                // detail budget is exhausted.
                ++result.omittedOrphanDiagnosticCount_;
            }
            drop = true;
        }
        if (!drop) {
            result.mask_->set(ordinal);
        }
        return drop;
    });
    if (result.omittedOrphanDiagnosticCount_ != 0) {
        result.diags_.warn(codes::kWarnSelectionOrphan,
                           std::format("{} additional orphan warnings omitted (detail limit {}); "
                                       "{} orphan warnings in total",
                                       result.omittedOrphanDiagnosticCount_,
                                       *options_.orphanDiagnosticLimit, result.orphanWarningCount_),
                           "selection");
    }
    result.mask_->finish();
    return result;
}

SelectedOccurrences SelectedOccurrences::prune(const std::vector<config::SelectionRule> &rules,
                                               bool hoist) const {
    if (rules.empty()) {
        // A valid existing topology already satisfies the descendant invariant.
        // Keep an implicit all-present view implicit, even for enormous sources.
        return SelectedOccurrences{index_, mask_, {}, options_};
    }
    auto evaluated = dryRun(rules, hoist);
    if (hoist) {
        evaluated.mask_->set(0);
        evaluated.mask_->finish();
    } else if (!evaluated.mask_->contains(0)) {
        throw Error{codes::kFatalSelectionRootDropped,
                    "root node is in the dropped set; pruning is a no-op",
                    index_->tree.displayNameFor({}, index_->tree.rootPlacement().name),
                    evaluated.diags_};
    }
    return SelectedOccurrences{
        index_,   std::move(evaluated.mask_),    std::move(evaluated.diags_),
        options_, evaluated.orphanWarningCount_, evaluated.omittedOrphanDiagnosticCount_};
}

SelectedOccurrence SelectedOccurrences::materialize(const OccurrenceId &id) const {
    const auto [ordinal, end] = index_->interval(id);
    if (mask_ && !mask_->contains(ordinal)) {
        throw std::out_of_range("occurrence was removed by selection");
    }
    SelectedOccurrence result;
    result.source = index_->tree.materialize(id);
    result.localTransform = result.source->localTransform;
    result.isLeaf = mask_ ? !mask_->any(ordinal + 1, end) : result.source->childCount == 0;
    if (!id.empty()) {
        auto parent = id;
        parent.pop_back();
        const auto originalParent = parent;
        if (mask_) {
            // Resolve all source ancestor ordinals once, retaining the nearest
            // present one. Re-resolving each prefix would be quadratic in depth.
            std::size_t nearest = 0;
            uint64_t ancestorOrdinal = 0;
            auto volume = index_->tree.rootPlacement().logVolId;
            for (std::size_t depth = 0; depth < originalParent.size(); ++depth) {
                const auto child = originalParent.at(depth);
                ancestorOrdinal += index_->childOffsets.at(volume).at(child);
                volume = index_->tree.sourceScene().logVols.at(volume).daughters.at(child).logVolId;
                if (mask_->contains(ancestorOrdinal)) {
                    nearest = depth + 1;
                }
            }
            parent.resize(nearest);
        }
        if (parent != originalParent) {
            result.localTransform =
                glm::affineInverse(index_->tree.materialize(parent)->worldTransform) *
                result.source->worldTransform;
        }
        result.parentId = std::move(parent);
    }
    return result;
}

} // namespace nodehammer::selection
