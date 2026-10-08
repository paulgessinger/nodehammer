#pragma once

#include <ir/semantic/importer.hpp>

namespace nodehammer::ir {

// Native main installs this factory once, before command dispatch or worker
// threads. Library callers keep the ordinary private-detector importer. The
// hook lives in the full registry, not in the minimal ingestion SDK.
using DD4hepImporterFactory = std::unique_ptr<ISemanticImporter> (*)();
void setNativeDD4hepImporterFactory(DD4hepImporterFactory factory);

} // namespace nodehammer::ir
