#pragma once

// Where a public handle's `Impl` is defined, and the small helpers that build
// and read one.
//
// There is no access mechanism here, because none is needed: each public class
// stores its `shared_ptr<const Impl>` as a public member, and what makes the
// handle opaque is that `Impl` is defined *here* — in a header that is never
// installed — rather than in the class. So the bridge translation units simply
// touch `handle.impl` like any other member, and these functions exist only to
// keep the `make_shared` incantation and the null checks in one place.
//
// Everything is inline and un-decorated, so none of it is exported.

#include <config/config_ast.hpp>
#include <diagnostics.hpp>
#include <ir/render.hpp>
#include <ir/semantic.hpp>

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>

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

namespace api {

// ── Diagnostics ──────────────────────────────────────────────────────────────

// The severity enums are two spellings of one ladder, and the conversion is a
// cast rather than a switch only because these asserts hold. If the internal
// enum ever grows a level, this is where it fails.
static_assert(static_cast<int>(diagnostics::DiagnosticSeverity::Debug) ==
              static_cast<int>(Diagnostic::Severity::Debug));
static_assert(static_cast<int>(diagnostics::DiagnosticSeverity::Info) ==
              static_cast<int>(Diagnostic::Severity::Info));
static_assert(static_cast<int>(diagnostics::DiagnosticSeverity::Warning) ==
              static_cast<int>(Diagnostic::Severity::Warning));
static_assert(static_cast<int>(diagnostics::DiagnosticSeverity::Error) ==
              static_cast<int>(Diagnostic::Severity::Error));
static_assert(static_cast<int>(diagnostics::DiagnosticSeverity::Fatal) ==
              static_cast<int>(Diagnostic::Severity::Fatal));

[[nodiscard]] inline Diagnostic::Severity severity(diagnostics::DiagnosticSeverity s) noexcept {
    return static_cast<Diagnostic::Severity>(
        static_cast<std::underlying_type_t<diagnostics::DiagnosticSeverity>>(s));
}

/// Convert an internal list to public items. Accumulate these, then `seal` once
/// — a `DiagnosticList` is immutable, so it is built as a vector and frozen
/// rather than appended to in place.
[[nodiscard]] inline std::vector<Diagnostic> items(const diagnostics::DiagnosticList &src) {
    std::vector<Diagnostic> out;
    out.reserve(src.items().size());
    for (const auto &d : src.items()) {
        out.push_back(Diagnostic{severity(d.severity), d.code, d.message, d.context});
    }
    return out;
}

inline void appendTo(std::vector<Diagnostic> &dst, const diagnostics::DiagnosticList &src) {
    dst.reserve(dst.size() + src.items().size());
    for (const auto &d : src.items()) {
        dst.push_back(Diagnostic{severity(d.severity), d.code, d.message, d.context});
    }
}

/// Freeze accumulated items into the public list. An empty list allocates
/// nothing — which is what every successful call returns.
[[nodiscard]] inline DiagnosticList seal(std::vector<Diagnostic> items) {
    DiagnosticList out;
    if (!items.empty()) {
        out.impl =
            std::make_shared<const DiagnosticList::Impl>(DiagnosticList::Impl{std::move(items)});
    }
    return out;
}

[[nodiscard]] inline DiagnosticList wrap(const diagnostics::DiagnosticList &src) {
    return seal(items(src));
}

/// The single-error list a bridge failure returns. Spelled out here so no verb
/// has to remember the three-line dance.
[[nodiscard]] inline DiagnosticList error(std::string_view code, std::string_view message,
                                          std::string_view context = {}) {
    std::vector<Diagnostic> one;
    one.push_back(Diagnostic{Diagnostic::Severity::Error, std::string{code}, std::string{message},
                             std::string{context}});
    return seal(std::move(one));
}

// ── Scenes ───────────────────────────────────────────────────────────────────

[[nodiscard]] inline SemanticScene wrap(ir::semantic::Scene scene) {
    SemanticScene handle;
    handle.impl =
        std::make_shared<const SemanticScene::Impl>(SemanticScene::Impl{std::move(scene)});
    return handle;
}

/// Null when the handle refers to nothing. Every verb checks this first — an
/// invalid handle is a diagnostic, never a crash.
[[nodiscard]] inline const ir::semantic::Scene *sceneOf(const SemanticScene &handle) noexcept {
    return handle.impl ? &handle.impl->scene : nullptr;
}

[[nodiscard]] inline RenderScene wrap(ir::render::Scene scene) {
    RenderScene handle;
    handle.impl = std::make_shared<const RenderScene::Impl>(RenderScene::Impl{std::move(scene)});
    return handle;
}

[[nodiscard]] inline const ir::render::Scene *sceneOf(const RenderScene &handle) noexcept {
    return handle.impl ? &handle.impl->scene : nullptr;
}

// ── Configs ──────────────────────────────────────────────────────────────────

[[nodiscard]] inline Config wrap(config::NHConfig cfg) {
    Config handle;
    handle.impl = std::make_shared<const Config::Impl>(Config::Impl{std::move(cfg)});
    return handle;
}

[[nodiscard]] inline const config::NHConfig *configOf(const Config &handle) noexcept {
    return handle.impl ? &handle.impl->cfg : nullptr;
}

/// The document a slice was cut from. Unlike the scene handles this never
/// returns null: a default-constructed slice legitimately means "no config
/// file", which is the default-constructed AST, and every consumer of a slice
/// already treats that as valid input.
[[nodiscard]] inline const config::NHConfig &configOf(const SceneConfig &slice) noexcept {
    static const config::NHConfig kDefaults{};
    return slice.impl && slice.impl->cfg ? *slice.impl->cfg : kDefaults;
}

[[nodiscard]] inline const config::NHConfig &configOf(const OutputConfig &slice) noexcept {
    static const config::NHConfig kDefaults{};
    return slice.impl && slice.impl->cfg ? *slice.impl->cfg : kDefaults;
}

/// An aliasing pointer into a `Config`'s Impl: a slice shares ownership of the
/// whole handle while pointing at just the AST, so slicing costs one control-
/// block bump and no copy of the document.
[[nodiscard]] inline std::shared_ptr<const config::NHConfig>
documentOf(const Config &handle) noexcept {
    return handle.impl ? std::shared_ptr<const config::NHConfig>{handle.impl, &handle.impl->cfg}
                       : std::shared_ptr<const config::NHConfig>{};
}

[[nodiscard]] inline SceneConfig sceneSlice(std::shared_ptr<const config::NHConfig> cfg) {
    SceneConfig slice;
    slice.impl = std::make_shared<const SceneConfig::Impl>(SceneConfig::Impl{std::move(cfg)});
    return slice;
}

[[nodiscard]] inline OutputConfig outputSlice(std::shared_ptr<const config::NHConfig> cfg) {
    OutputConfig slice;
    slice.impl = std::make_shared<const OutputConfig::Impl>(OutputConfig::Impl{std::move(cfg)});
    return slice;
}

} // namespace api
} // namespace nodehammer
