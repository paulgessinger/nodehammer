#pragma once

// The config handles' `Impl`s, and the helpers that build and read one.
//
// Split out of handles.hpp so a TU that does not touch the config AST does not
// pay for `config/config_ast.hpp`. See the note there.

#include <api/handles.hpp>

#include <config/config_ast.hpp>
#include <nodehammer/config.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace nodehammer {

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

} // namespace nodehammer

namespace nodehammer::api {

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

} // namespace nodehammer::api
