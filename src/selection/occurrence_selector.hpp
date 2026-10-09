#pragma once

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/semantic/occurrence_tree.hpp>

namespace nodehammer::selection {

using ir::semantic::Occurrence;
using ir::semantic::OccurrenceId;
using ir::semantic::OccurrenceTree;

namespace detail {
struct OccurrenceIndex;
struct OccurrenceMask;
} // namespace detail

struct OccurrenceSelectionOptions {
    // nullopt explicitly requests every detail. The default bounds retained
    // orphan messages; exact warning counts are independent of this limit.
    std::optional<uint64_t> orphanDiagnosticLimit{1024};
};

class OccurrenceEvaluation {
  public:
    [[nodiscard]] uint64_t keptCount() const;
    [[nodiscard]] uint64_t droppedCount() const;
    [[nodiscard]] bool contains(const OccurrenceId &id) const;
    [[nodiscard]] const DiagnosticList &diagnostics() const { return diags_; }
    [[nodiscard]] uint64_t orphanWarningCount() const { return orphanWarningCount_; }
    [[nodiscard]] uint64_t omittedOrphanDiagnosticCount() const {
        return omittedOrphanDiagnosticCount_;
    }

  private:
    friend class SelectedOccurrences;
    OccurrenceEvaluation() = default;
    std::shared_ptr<const detail::OccurrenceIndex> index_;
    std::shared_ptr<detail::OccurrenceMask> mask_;
    uint64_t inputCount_{};
    DiagnosticList diags_;
    uint64_t orphanWarningCount_{};
    uint64_t omittedOrphanDiagnosticCount_{};
};

struct SelectedOccurrence {
    std::shared_ptr<const Occurrence> source;
    std::optional<OccurrenceId> parentId;
    glm::dmat4 localTransform{1.0};
    bool isLeaf{};
};

// Experimental selection over immutable source prototypes. Source ordinals are
// implicit DFS indices, never NodeIds. Persistent topology uses one bit per source
// occurrence plus a word-level rank index, not allocated Node objects. Snapshots
// are materialized only on demand. Successive filters cannot resurrect removals.
//
// Tree and source must outlive all views/results; do not mutate metadata or prototypes
// while any view is in use. Cache eviction is allowed. The processing pipeline
// materializes retained occurrences after this selection stage.
class SelectedOccurrences {
  public:
    explicit SelectedOccurrences(OccurrenceTree &tree, OccurrenceSelectionOptions options = {});
    [[nodiscard]] OccurrenceEvaluation dryRun(const std::vector<config::SelectionRule> &rules,
                                              bool hoist = false) const;
    [[nodiscard]] SelectedOccurrences prune(const std::vector<config::SelectionRule> &rules,
                                            bool hoist = false) const;
    [[nodiscard]] uint64_t size() const;
    // Retained occurrences in a source subtree, including its root.
    [[nodiscard]] uint64_t subtreeSize(const OccurrenceId &id) const;
    [[nodiscard]] bool contains(const OccurrenceId &id) const;
    [[nodiscard]] SelectedOccurrence materialize(const OccurrenceId &id) const;
    // Retained mask + rank array payload, excluding prototypes, tags, caches,
    // diagnostics and allocator/container overhead. Initially-all-present is zero.
    [[nodiscard]] std::size_t storageBytes() const;
    [[nodiscard]] const DiagnosticList &diagnostics() const { return diags_; }
    [[nodiscard]] uint64_t orphanWarningCount() const { return orphanWarningCount_; }
    [[nodiscard]] uint64_t omittedOrphanDiagnosticCount() const {
        return omittedOrphanDiagnosticCount_;
    }

  private:
    SelectedOccurrences(std::shared_ptr<const detail::OccurrenceIndex> index,
                        std::shared_ptr<const detail::OccurrenceMask> mask, DiagnosticList diags,
                        OccurrenceSelectionOptions options, uint64_t orphanWarningCount = 0,
                        uint64_t omittedOrphanDiagnosticCount = 0);
    std::shared_ptr<const detail::OccurrenceIndex> index_;
    std::shared_ptr<const detail::OccurrenceMask> mask_;
    OccurrenceSelectionOptions options_;
    DiagnosticList diags_;
    uint64_t orphanWarningCount_{};
    uint64_t omittedOrphanDiagnosticCount_{};
};

} // namespace nodehammer::selection
