#pragma once

#include <ir/semantic/importer.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace nodehammer::ir {

/// ISemanticImporter that reads a FlatBuffer-encoded expanded::Scene (.nhb/.nhb.zst).
/// Format name: "nhb"   Extensions: "nhb", "nhb.zst"
class FlatBufferImporter final : public ISemanticImporter {
  public:
    [[nodiscard]] std::string_view formatName() const noexcept override;
    [[nodiscard]] std::vector<std::string> supportedExtensions() const override;
    [[nodiscard]] ImportResult import(const std::filesystem::path &path) const override;

    /// Bytes-only entry point. `filename` is used purely for diagnostics
    /// (and as a `.zst` hint); zstd frames are also detected from their magic.
    /// The actual data comes from `bytes`. Used by the viewer build
    /// pipeline so geometry can flow straight from a project's resolved
    /// bytes into a expanded::Scene without a filesystem round-trip.
    [[nodiscard]] static ImportResult importFromBytes(std::string_view filename,
                                                      std::span<const std::byte> bytes);
};

} // namespace nodehammer::ir
