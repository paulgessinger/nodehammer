#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace nodehammer {
/// Primitive option values. Types are checked exactly, without coercion.
using ImporterOptionValue = std::variant<bool, std::int64_t, double, std::string>;
/// Backend-qualified names, e.g. "dd4hep.useGlobalDetector".
using ImporterOptions = std::map<std::string, ImporterOptionValue, std::less<>>;
struct SemanticReadOptions {
    /// Empty means infer from the filename; otherwise select this importer.
    std::string format{};
    /// Unknown names, incorrect types, and options for another backend are errors.
    ImporterOptions importerOptions{};
};
} // namespace nodehammer
