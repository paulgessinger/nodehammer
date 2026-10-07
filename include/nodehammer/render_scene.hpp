#pragma once

#include <cstddef>
#include <memory>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/visibility.hpp>

namespace nodehammer {

/// Immutable, cheaply copyable geometry handle. A default or moved-from handle
/// is empty; conversion and processing functions reject empty handles.
class RenderScene {
  public:
    [[nodiscard]] NH_API bool valid() const noexcept;
    [[nodiscard]] NH_API std::size_t nodeCount() const noexcept;
    [[nodiscard]] NH_API std::size_t meshCount() const noexcept;
    [[nodiscard]] NH_API std::size_t materialCount() const noexcept;
    [[nodiscard]] NH_API std::size_t triangleCount() const noexcept;

    // Private implementation access; defined only inside the library.
    struct Impl;

    RenderScene() noexcept = default;

    /// Adopt state the library built.
    explicit RenderScene(std::shared_ptr<const Impl> impl) noexcept;

    /// The state behind a live handle. Throws `Error` when `valid()` is false.
    [[nodiscard]] const Impl &impl() const;

  private:
    std::shared_ptr<const Impl> impl_;
};

struct RenderResult {
    RenderScene scene;
    DiagnosticList diags;
};

} // namespace nodehammer
