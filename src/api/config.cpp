#include <api/handles.hpp>

#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <detail/file_io.hpp>
#include <diagnostic_codes.hpp>

#if NH_WITH_LUA
#include <lua/lua_config.hpp>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <utility>

namespace nodehammer {
namespace {

/// Adopt an internal load, then validate — the one thing these wrappers add
/// beyond dispatch and slicing.
///
/// Validation is skipped after a failed load, matching `convert`: a load error
/// leaves a default-constructed AST, and validating that would answer a
/// question about a document nobody wrote. Validation *diagnostics* land in the
/// same list as the load's, which is what makes `build` free of a validation
/// step of its own (#41 §8).
[[nodiscard]] ConfigResult adopt(config::ConfigResult loaded, const std::string &context) {
    // Accumulated in the internal list, which is the type that knows how to
    // append, and converted once at the end.
    diagnostics::List diags = std::move(loaded.diags);
    diagnostics::throwIfErrors(diags, context);
    diags.append(config::ConfigValidator::validate(loaded.config));
    diagnostics::throwIfErrors(diags, context);
    return ConfigResult{api::asHandle(std::move(loaded.config)),
                        diagnostics::asHandle(std::move(diags))};
}

/// Cut a slice from a document. The pointer aliases the handle's state, so a
/// slice shares ownership of the whole thing while pointing at just the AST:
/// slicing costs one control-block bump and no copy of the document.
///
/// Takes the state rather than the handle, since reaching a handle's state as a
/// *pointer* — rather than through `impl()`, which hands out a reference — is
/// something only a member can do, and both callers are members. `state` must
/// be non-null, which is what makes a slice's own `cfg` non-null whenever it
/// has state at all.
template <typename Slice>
[[nodiscard]] Slice slice(const std::shared_ptr<const Config::Impl> &state) {
    return Slice{std::make_shared<const typename Slice::Impl>(
        typename Slice::Impl{std::shared_ptr<const config::NHConfig>{state, &state->cfg}})};
}

[[nodiscard]] bool hasLuaExtension(const std::filesystem::path &path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".lua";
}

} // namespace

ConfigResult Config::read(const std::filesystem::path &path) {
    if (!hasLuaExtension(path)) {
        return adopt(config::ConfigLoader::loadFromFile(path), path.string());
    }

#if NH_WITH_LUA
    // Canonicalise first, then take the parent: that is what roots `include()`
    // and `use()`, and it is derived from the caller's own path rather than
    // invented. A bare `cfg.lua` therefore roots at the directory the file was
    // actually found in, without this layer ever deciding what "here" means
    // (#41 §11, conventions from step 5b).
    std::filesystem::path canonical;
    std::string source;
    try {
        canonical = std::filesystem::canonical(path);
        const auto bytes = detail::file_io::readFile(canonical);
        source.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, path.string());
    }
    try {
        return adopt(lua::evalLuaConfig(source, canonical.string(), canonical.parent_path()),
                     path.string());
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrConfigParse, path.string());
    }
#else
    throw Error{codes::kFatalApiBackendMissing,
                "this build has no Lua config front end; see formats()", path.string()};
#endif
}

ConfigResult Config::parse(std::string_view toml, const std::filesystem::path &baseDir) {
    try {
        return adopt(config::ConfigLoader::loadFromString(toml, "<string>", baseDir), "<string>");
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kErrConfigParse, "<string>");
    }
}

std::span<const std::string_view> Config::formats() {
    static constexpr std::string_view kToml{"toml"};
#if NH_WITH_LUA
    static constexpr std::array<std::string_view, 2> kFormats{kToml, std::string_view{"lua"}};
#else
    static constexpr std::array<std::string_view, 1> kFormats{kToml};
#endif
    return kFormats;
}

// Slicing an empty handle yields an empty slice rather than a slice of nothing.
// The two would answer every public question identically — `valid()` is false
// and the resolvers fall back to the built-in defaults either way — but only
// this one leaves a slice with a single way to mean "no document", so `cfg` is
// non-null wherever a slice has state at all.

SceneConfig Config::scene() const { return impl_ ? slice<SceneConfig>(impl_) : SceneConfig{}; }

OutputConfig Config::output() const { return impl_ ? slice<OutputConfig>(impl_) : OutputConfig{}; }

bool Config::valid() const noexcept { return impl_ != nullptr; }

bool SceneConfig::valid() const noexcept { return impl_ != nullptr; }

bool OutputConfig::valid() const noexcept { return impl_ != nullptr; }

} // namespace nodehammer
