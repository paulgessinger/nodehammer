#pragma once

// The one place where a public handle and its internals are both visible.
//
// Every public class in include/nodehammer/ declares `struct Impl;` privately
// and befriends `api::Access`. This header defines those Impls and gives the
// bridge translation units — and nothing else — the wrap/unwrap pair for each.
// Keeping it to one header is what stops the seam from being re-invented per
// verb: there is exactly one way to get an `ir::semantic::Scene` out of a
// `SemanticScene`, and it is `Access::sceneOf`.
//
// Everything here is inline and un-decorated, so none of it is exported: the
// seam exists at compile time and leaves no symbol behind.

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

struct Access {
    // ── Diagnostics ──────────────────────────────────────────────────────────

    [[nodiscard]] static Diagnostic::Severity severity(diagnostics::DiagnosticSeverity s) noexcept {
        return static_cast<Diagnostic::Severity>(
            static_cast<std::underlying_type_t<diagnostics::DiagnosticSeverity>>(s));
    }

    /// Convert an internal list to public items. Accumulate these, then `seal`
    /// once — a `DiagnosticList` is immutable, so it is built as a vector and
    /// frozen rather than appended to in place.
    [[nodiscard]] static std::vector<Diagnostic> items(const diagnostics::DiagnosticList &src) {
        std::vector<Diagnostic> out;
        out.reserve(src.items().size());
        for (const auto &d : src.items()) {
            out.push_back(Diagnostic{severity(d.severity), d.code, d.message, d.context});
        }
        return out;
    }

    static void appendTo(std::vector<Diagnostic> &dst, const diagnostics::DiagnosticList &src) {
        dst.reserve(dst.size() + src.items().size());
        for (const auto &d : src.items()) {
            dst.push_back(Diagnostic{severity(d.severity), d.code, d.message, d.context});
        }
    }

    /// Freeze accumulated items into the public list. An empty list allocates
    /// nothing — which is what every successful call returns.
    [[nodiscard]] static DiagnosticList seal(std::vector<Diagnostic> items) {
        DiagnosticList out;
        if (!items.empty()) {
            out.impl_ = std::make_shared<const DiagnosticList::Impl>(
                DiagnosticList::Impl{std::move(items)});
        }
        return out;
    }

    [[nodiscard]] static DiagnosticList wrap(const diagnostics::DiagnosticList &src) {
        return seal(items(src));
    }

    /// The single-error list a bridge failure returns. Spelled out here so no
    /// verb has to remember the three-line dance.
    [[nodiscard]] static DiagnosticList error(std::string_view code, std::string_view message,
                                              std::string_view context = {}) {
        std::vector<Diagnostic> one;
        one.push_back(Diagnostic{Diagnostic::Severity::Error, std::string{code},
                                 std::string{message}, std::string{context}});
        return seal(std::move(one));
    }

    // ── Semantic scenes ──────────────────────────────────────────────────────

    [[nodiscard]] static SemanticScene wrap(ir::semantic::Scene scene) {
        SemanticScene handle;
        handle.impl_ =
            std::make_shared<const SemanticScene::Impl>(SemanticScene::Impl{std::move(scene)});
        return handle;
    }

    /// Null when the handle refers to nothing. Every verb checks this first —
    /// an invalid handle is a diagnostic, never a crash.
    [[nodiscard]] static const ir::semantic::Scene *sceneOf(const SemanticScene &handle) noexcept {
        return handle.impl_ ? &handle.impl_->scene : nullptr;
    }

    // ── Render scenes ────────────────────────────────────────────────────────

    [[nodiscard]] static RenderScene wrap(ir::render::Scene scene) {
        RenderScene handle;
        handle.impl_ =
            std::make_shared<const RenderScene::Impl>(RenderScene::Impl{std::move(scene)});
        return handle;
    }

    [[nodiscard]] static const ir::render::Scene *sceneOf(const RenderScene &handle) noexcept {
        return handle.impl_ ? &handle.impl_->scene : nullptr;
    }

    // ── Configs ──────────────────────────────────────────────────────────────

    [[nodiscard]] static Config wrap(config::NHConfig cfg) {
        Config handle;
        handle.impl_ = std::make_shared<const Config::Impl>(Config::Impl{std::move(cfg)});
        return handle;
    }

    [[nodiscard]] static const config::NHConfig *configOf(const Config &handle) noexcept {
        return handle.impl_ ? &handle.impl_->cfg : nullptr;
    }

    /// The document a slice was cut from. Unlike the scene handles this never
    /// returns null: a default-constructed slice legitimately means "no config
    /// file", which is the default-constructed AST, and every consumer of a
    /// slice already treats that as valid input.
    [[nodiscard]] static const config::NHConfig &configOf(const SceneConfig &slice) noexcept {
        static const config::NHConfig kDefaults{};
        return slice.impl_ && slice.impl_->cfg ? *slice.impl_->cfg : kDefaults;
    }

    [[nodiscard]] static const config::NHConfig &configOf(const OutputConfig &slice) noexcept {
        static const config::NHConfig kDefaults{};
        return slice.impl_ && slice.impl_->cfg ? *slice.impl_->cfg : kDefaults;
    }

    [[nodiscard]] static SceneConfig sceneSlice(std::shared_ptr<const config::NHConfig> cfg) {
        SceneConfig slice;
        slice.impl_ = std::make_shared<const SceneConfig::Impl>(SceneConfig::Impl{std::move(cfg)});
        return slice;
    }

    [[nodiscard]] static OutputConfig outputSlice(std::shared_ptr<const config::NHConfig> cfg) {
        OutputConfig slice;
        slice.impl_ =
            std::make_shared<const OutputConfig::Impl>(OutputConfig::Impl{std::move(cfg)});
        return slice;
    }

    /// An aliasing pointer into a `Config`'s Impl: the slice shares ownership of
    /// the whole handle while pointing at just the AST, so slicing costs one
    /// control-block bump and no copy of the document.
    [[nodiscard]] static std::shared_ptr<const config::NHConfig>
    documentOf(const std::shared_ptr<const Config::Impl> &impl) noexcept {
        return impl ? std::shared_ptr<const config::NHConfig>{impl, &impl->cfg}
                    : std::shared_ptr<const config::NHConfig>{};
    }

    [[nodiscard]] static const std::shared_ptr<const Config::Impl> &
    implOf(const Config &handle) noexcept {
        return handle.impl_;
    }
};

} // namespace api
} // namespace nodehammer
