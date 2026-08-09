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

inline RenderScene::RenderScene(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const RenderScene::Impl &RenderScene::impl() const {
    if (!impl_) {
        throw Error{codes::kFatalApiInvalidHandle, "the render scene handle refers to nothing"};
    }
    return *impl_;
}

inline Config::Config(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl)) {}

inline const Config::Impl &Config::impl() const {
    if (!impl_) {
        throw Error{codes::kFatalApiInvalidHandle, "the config handle refers to nothing"};
    }
    return *impl_;
}

inline SceneConfig::SceneConfig(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const SceneConfig::Impl &SceneConfig::impl() const {
    if (!impl_) {
        throw Error{codes::kFatalApiInvalidHandle, "the scene config slice refers to nothing"};
    }
    return *impl_;
}

inline OutputConfig::OutputConfig(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

inline const OutputConfig::Impl &OutputConfig::impl() const {
    if (!impl_) {
        throw Error{codes::kFatalApiInvalidHandle, "the output config slice refers to nothing"};
    }
    return *impl_;
}

namespace api {

// The seam itself: eleven functions, in two directions.
//
//   asHandle(value)        an internal value → the public handle for it.
//                          Overloaded, since there is one handle type per
//                          internal type and nothing to choose between them.
//                          The diagnostics overload lives in src/diagnostics.hpp,
//                          where the failure channel needs it too.
//   sceneOrThrow(h, verb)  a public handle → the scene inside it, or `Error`
//                          naming the verb that was handed an empty handle.
//   documentOf(slice)      a config slice → the parsed document, or the
//                          built-in defaults.
//   rethrowAsError(...)    a third-party exception → a failure this library can
//                          name.
//
// The suffix carries the failure mode, which is the thing a reader at the call
// site actually needs: `…OrThrow` throws on an empty handle, `…Of` cannot fail,
// and `throw…` never returns. Nothing here is a bare verb whose behaviour you
// have to come back to this file to learn.

// ── Scenes ───────────────────────────────────────────────────────────────────

[[nodiscard]] inline SemanticScene asHandle(ir::semantic::Scene scene) {
    return SemanticScene{
        std::make_shared<const SemanticScene::Impl>(SemanticScene::Impl{std::move(scene)})};
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

[[nodiscard]] inline RenderScene asHandle(ir::render::Scene scene) {
    return RenderScene{
        std::make_shared<const RenderScene::Impl>(RenderScene::Impl{std::move(scene)})};
}

[[nodiscard]] inline const ir::render::Scene &sceneOrThrow(const RenderScene &handle,
                                                           std::string_view verb) {
    if (!handle.valid()) {
        throw Error{codes::kFatalApiInvalidHandle, "the render scene handle refers to nothing",
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

// ── Configs ──────────────────────────────────────────────────────────────────

[[nodiscard]] inline Config asHandle(config::NHConfig cfg) {
    return Config{std::make_shared<const Config::Impl>(Config::Impl{std::move(cfg)})};
}

/// The document a slice was cut from. `…Of` rather than `…OrThrow` because a
/// slice with no document legitimately means "no config file", which is the
/// default-constructed AST, and every consumer of a slice already treats that
/// as valid input.
///
/// A slice is the one state here that something other than its own members
/// reads — the verbs in build.cpp and `RenderScene::write` — which is why these
/// two are free functions at all.
[[nodiscard]] inline const config::NHConfig &documentOf(const SceneConfig &slice) noexcept {
    static const config::NHConfig kDefaults{};
    return slice.valid() ? *slice.impl().cfg : kDefaults;
}

[[nodiscard]] inline const config::NHConfig &documentOf(const OutputConfig &slice) noexcept {
    static const config::NHConfig kDefaults{};
    return slice.valid() ? *slice.impl().cfg : kDefaults;
}

} // namespace api
} // namespace nodehammer
