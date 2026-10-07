#pragma once

#include <cstddef>
#include <memory>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/visibility.hpp>

namespace nodehammer {

/// Immutable, cheaply copyable geometry handle. A default or moved-from handle
/// is empty; conversion and processing functions reject empty handles.
class SemanticScene {
  public:
    [[nodiscard]] NH_API bool valid() const noexcept;
    [[nodiscard]] NH_API std::size_t nodeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t logVolCount() const noexcept;
    [[nodiscard]] NH_API std::size_t shapeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t materialCount() const noexcept;

    // Private implementation access; defined only inside the library.
    struct Impl;

    SemanticScene() noexcept = default;

    /// Adopt state the library built.
    explicit SemanticScene(std::shared_ptr<const Impl> impl) noexcept;

    /// The state behind a live handle. Throws `Error` when `valid()` is false,
    /// since there is nothing to return a reference to.
    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
};

/// Named after its type rather than a generic `scene`, so a structured binding
/// reads correctly at the call site (#41 §9).
struct SemanticResult {
    SemanticScene scene;
    DiagnosticList diags;
};

} // namespace nodehammer
