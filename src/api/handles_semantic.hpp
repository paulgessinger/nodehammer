#pragma once

// Private semantic handle implementation and conversions. Keep configuration
// and rendering dependencies in their own handle headers.

#include <diagnostic_codes.hpp>
#include <diagnostics.hpp>
#include <ir/semantic/importer.hpp>

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <exception>
#include <memory>
#include <string_view>
#include <utility>

namespace nodehammer {

struct SemanticScene::Impl {
    ir::semantic::Scene scene;
    uint64_t occurrenceCount;
    explicit Impl(ir::semantic::Scene input)
        : scene(std::move(input)), occurrenceCount(scene.nodeCount()) {}
};

// ── The members that mention an Impl ─────────────────────────────────────────
//
// Each getter throws rather than dereferencing a null pointer, so "the handle
// refers to nothing" has one answer per type instead of one per call site. The
// verbs never reach it: `api::sceneOrThrow` below asks first, because it can say
// which verb was handed the empty handle and this cannot.

inline SemanticScene::SemanticScene(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const SemanticScene::Impl &SemanticScene::impl() const {
    if (!impl_) {
        throw Error{codes::kFatalApiInvalidHandle, "the semantic scene handle refers to nothing"};
    }
    return *impl_;
}

namespace api {

[[nodiscard]] inline SemanticScene asHandle(ir::semantic::Scene scene) {
    return SemanticScene{
        std::make_shared<const SemanticScene::Impl>(SemanticScene::Impl{std::move(scene)})};
}

// Explicit adapter for callers constructing physical processing fixtures.
[[nodiscard]] inline SemanticResult asHandle(ir::ImportResult result) {
    return {asHandle(std::move(result.scene)), std::move(result.diags)};
}

/// The scene behind a handle, or an `Error` naming the caller that was handed
/// nothing. Every *verb* starts here — a handle's own observers ask `impl_`
/// directly, being members. An empty handle is a caller mistake, and a mistake
/// with no result to report is what the exception channel is for.
///
/// All this adds over `handle.impl()`, which throws on its own, is the verb
/// name: an exception that says which call the caller got wrong, rather than
/// only which type, is worth one wrapper.
[[nodiscard]] inline const ir::semantic::Scene &sceneOrThrow(const SemanticScene &handle,
                                                             std::string_view verb) {
    if (!handle.valid()) {
        throw Error{codes::kFatalApiInvalidHandle, "the semantic scene handle refers to nothing",
                    verb};
    }
    return handle.impl().scene;
}

/// Rethrow whatever escaped an internal call as the one type that crosses this
/// API. Internal code throws `std::runtime_error` from the codecs and the file
/// helpers; none of those types is part of the contract.
[[noreturn]] inline void rethrowAsError(const std::exception &e, std::string_view code,
                                        std::string_view context = {}) {
    throw Error{code, e.what(), context};
}

} // namespace api
} // namespace nodehammer
