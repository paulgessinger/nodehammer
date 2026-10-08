#pragma once

#include <ir/semantic/importer.hpp>

#include <string>

namespace dd4hep {
class Detector;
}

namespace nodehammer::ir::semantic {

/// Borrow a constructed detector; preserve source prototypes and sparse metadata.
/// Does not load XML, use the global detector, expand Nodes, or transfer ownership.
/// Unresolved/ambiguous DetElement occurrences are reported as errors alongside
/// the imported geometry, rather than silently assigning metadata to one copy.
[[nodiscard]] ir::ImportResult importDD4hep(dd4hep::Detector &detector,
                                            std::string sourceFile = {});

} // namespace nodehammer::ir::semantic
