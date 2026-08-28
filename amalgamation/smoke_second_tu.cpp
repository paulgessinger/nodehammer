// A second translation unit that includes the header *without* defining
// NH_IMPLEMENTATION -- the ordinary case, and the one a consumer has many of.
//
// Linked against smoke.cpp, this is what proves the split holds. If anything in
// the interface section were a non-inline definition rather than a declaration,
// two objects would carry it and the link would fail with a duplicate symbol.
// A single-TU test cannot see that, which is the whole reason this file exists.
//
// It also stands for the cost claim: including this header for declarations
// pulls in none of the vendored unordered_dense or flatbuffers, because all of
// it sits behind the NH_IMPLEMENTATION guard.

#include "nodehammer_connect.h"

#include <cstddef>
#include <vector>

namespace smoke {

/// Uses the declared API without ever seeing a definition of it.
std::size_t countNodes(const nodehammer::SemanticScene &scene) { return scene.nodeCount(); }

/// Names the result types too, so the whole declared vocabulary is instantiated
/// here and not just the one class.
bool describe(const nodehammer::SemanticResult &result) {
    return result.scene.valid() && !result.diags.hasErrors();
}

} // namespace smoke
