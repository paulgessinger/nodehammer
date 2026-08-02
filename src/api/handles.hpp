#pragma once

// Where a public handle's `Impl` is defined, and the small helpers that build
// and read one.
//
// This is also where the members that *mention* an `Impl` are defined — each
// handle's adopting constructor and its `impl()` getter. They have to live in a
// header that sees the definition, and this is the only one there is: it is
// never installed, which is the whole reason the handles are opaque. The state
// itself stays private in the public class, so the bridge's access to it is
// those two members and nothing else — no friend declaration, and no way for a
// caller to swap out state the handles promise never changes.
//
// Everything here is inline and un-decorated, so none of it is exported. A
// consumer compiling against the installed headers sees the declarations and
// finds no symbol behind them, which is correct: they have no `Impl` to pass in
// and could do nothing with one handed back.

#include <config/config_ast.hpp>
#include <diagnostic_codes.hpp>
#include <diagnostics.hpp>
#include <ir/render.hpp>
#include <ir/semantic.hpp>

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nodehammer {

struct DiagnosticList::Impl {
    std::vector<Diagnostic> items;
};

struct SemanticScene::Impl {
    ir::semantic::Scene scene;
};

struct RenderScene::Impl {
    ir::render::Scene scene;
};

struct Config::Impl {
    config::NHConfig cfg;
};

// The two slices hold the parsed document, not a copy of the fields they cover:
// slicing is about which half of the API may *read* what, and duplicating the
// AST to express that would put the two halves out of sync the moment one is
// rebuilt. `Config::scene()` hands over an aliasing pointer, so a slice keeps
// its document alive on its own.
struct SceneConfig::Impl {
    std::shared_ptr<const config::NHConfig> cfg;
};

struct OutputConfig::Impl {
    std::shared_ptr<const config::NHConfig> cfg;
};

// ── The members that mention an Impl ─────────────────────────────────────────
//
// Each getter throws rather than dereferencing a null pointer, so "the handle
// refers to nothing" has one answer per type instead of one per call site. The
// verbs never reach it: `api::require` below asks first, because it can say
// which verb was handed the empty handle and this cannot.

inline DiagnosticList::DiagnosticList(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline SemanticScene::SemanticScene(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const SemanticScene::Impl &SemanticScene::impl() const {
    if (!impl_) {
        throw Error{codes::kErrApiInvalidHandle, "the semantic scene handle refers to nothing"};
    }
    return *impl_;
}

inline RenderScene::RenderScene(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const RenderScene::Impl &RenderScene::impl() const {
    if (!impl_) {
        throw Error{codes::kErrApiInvalidHandle, "the render scene handle refers to nothing"};
    }
    return *impl_;
}

inline Config::Config(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl)) {}

inline const Config::Impl &Config::impl() const {
    if (!impl_) {
        throw Error{codes::kErrApiInvalidHandle, "the config handle refers to nothing"};
    }
    return *impl_;
}

inline SceneConfig::SceneConfig(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const SceneConfig::Impl &SceneConfig::impl() const {
    if (!impl_) {
        throw Error{codes::kErrApiInvalidHandle, "the scene config slice refers to nothing"};
    }
    return *impl_;
}

inline OutputConfig::OutputConfig(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const OutputConfig::Impl &OutputConfig::impl() const {
    if (!impl_) {
        throw Error{codes::kErrApiInvalidHandle, "the output config slice refers to nothing"};
    }
    return *impl_;
}

namespace api {

// ── Diagnostics ──────────────────────────────────────────────────────────────

/// Hand the internal list's items to the public one.
///
/// Both sides hold `std::vector<nodehammer::Diagnostic>` — the same type, since
/// the internal header now uses the published struct — so this moves rather than
/// converts, and an empty list allocates nothing.
[[nodiscard]] inline DiagnosticList wrap(diagnostics::List &&src) {
    std::vector<Diagnostic> items = std::move(src).take();
    if (items.empty()) {
        return DiagnosticList{};
    }
    return DiagnosticList{
        std::make_shared<const DiagnosticList::Impl>(DiagnosticList::Impl{std::move(items)})};
}

/// For a list the caller still needs afterwards.
[[nodiscard]] inline DiagnosticList wrap(const diagnostics::List &src) {
    diagnostics::List copy = src;
    return wrap(std::move(copy));
}

// ── Scenes ───────────────────────────────────────────────────────────────────

[[nodiscard]] inline SemanticScene wrap(ir::semantic::Scene scene) {
    return SemanticScene{
        std::make_shared<const SemanticScene::Impl>(SemanticScene::Impl{std::move(scene)})};
}

/// Null when the handle refers to nothing.
[[nodiscard]] inline const ir::semantic::Scene *sceneOf(const SemanticScene &handle) noexcept {
    return handle.valid() ? &handle.impl().scene : nullptr;
}

/// The scene behind a handle, or an `Error` naming the caller that was handed
/// nothing. Every verb starts here: an empty handle is a caller mistake, and a
/// mistake with no result to report is what the exception channel is for.
[[nodiscard]] inline const ir::semantic::Scene &require(const SemanticScene &handle,
                                                        std::string_view verb) {
    if (!handle.valid()) {
        throw Error{codes::kErrApiInvalidHandle, "the semantic scene handle refers to nothing",
                    verb};
    }
    return handle.impl().scene;
}

[[nodiscard]] inline RenderScene wrap(ir::render::Scene scene) {
    return RenderScene{
        std::make_shared<const RenderScene::Impl>(RenderScene::Impl{std::move(scene)})};
}

[[nodiscard]] inline const ir::render::Scene *sceneOf(const RenderScene &handle) noexcept {
    return handle.valid() ? &handle.impl().scene : nullptr;
}

[[nodiscard]] inline const ir::render::Scene &require(const RenderScene &handle,
                                                      std::string_view verb) {
    if (!handle.valid()) {
        throw Error{codes::kErrApiInvalidHandle, "the render scene handle refers to nothing", verb};
    }
    return handle.impl().scene;
}

/// Rethrow whatever escaped an internal call as the one type that crosses this
/// API. Internal code throws `std::runtime_error` from the codecs and the file
/// helpers; none of those types is part of the contract.
[[noreturn]] inline void rethrow(const std::exception &e, std::string_view code,
                                 std::string_view context = {}) {
    throw Error{code, e.what(), context};
}

/// Turn reported errors into the exception.
///
/// Some internal entry points report an unreadable input as an error
/// *diagnostic* rather than by throwing — every importer does, and so does the
/// config loader. At the boundary those are the same thing as a thrown failure:
/// input the call could not act on, with nothing usable produced. This is where
/// the two internal spellings become one external one.
///
/// The code and context come from the first error, since that is what a caller
/// would branch on; the message carries all of them, because a config with three
/// undefined material references should say so once rather than three calls in a
/// row.
[[noreturn]] inline void throwReported(const diagnostics::List &diags, std::string_view fallback,
                                       std::string_view context) {
    const Diagnostic *first = nullptr;
    std::string message;
    for (const auto &d : diags.items()) {
        if (d.severity < Diagnostic::Severity::Error) {
            continue;
        }
        if (first == nullptr) {
            first = &d;
            message = d.message;
            continue;
        }
        // Only once there is more than one does the message have to say which
        // code each part belongs to; a single failure already carries its code
        // on the exception.
        if (message == first->message) {
            message = first->code + ": " + first->message;
        }
        message += "; ";
        message += d.code;
        message += ": ";
        message += d.message;
    }
    if (first == nullptr) {
        throw Error{fallback, "the operation failed without saying why", context};
    }
    throw Error{first->code, message, first->context.empty() ? context : first->context};
}

// ── Configs ──────────────────────────────────────────────────────────────────

[[nodiscard]] inline Config wrap(config::NHConfig cfg) {
    return Config{std::make_shared<const Config::Impl>(Config::Impl{std::move(cfg)})};
}

[[nodiscard]] inline const config::NHConfig *configOf(const Config &handle) noexcept {
    return handle.valid() ? &handle.impl().cfg : nullptr;
}

/// The document a slice was cut from. Unlike the scene handles this never
/// returns null: a default-constructed slice legitimately means "no config
/// file", which is the default-constructed AST, and every consumer of a slice
/// already treats that as valid input.
[[nodiscard]] inline const config::NHConfig &configOf(const SceneConfig &slice) noexcept {
    static const config::NHConfig kDefaults{};
    return slice.valid() ? *slice.impl().cfg : kDefaults;
}

[[nodiscard]] inline const config::NHConfig &configOf(const OutputConfig &slice) noexcept {
    static const config::NHConfig kDefaults{};
    return slice.valid() ? *slice.impl().cfg : kDefaults;
}

} // namespace api
} // namespace nodehammer
