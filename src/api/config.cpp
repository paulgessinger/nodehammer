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
    if (diags.hasErrors()) {
        api::throwReported(diags, codes::kErrConfigParse, context);
    }
    diags.append(config::ConfigValidator::validate(loaded.config));
    if (diags.hasErrors()) {
        api::throwReported(diags, codes::kErrConfigParse, context);
    }
    return ConfigResult{api::wrap(std::move(loaded.config)), api::wrap(std::move(diags))};
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
        api::rethrow(e, codes::kErrImportFileNotFound, path.string());
    }
    try {
        return adopt(lua::evalLuaConfig(source, canonical.string(), canonical.parent_path()),
                     path.string());
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrow(e, codes::kErrConfigParse, path.string());
    }
#else
    throw Error{codes::kErrApiBackendMissing,
                "this build has no Lua config front end; see formats()", path.string()};
#endif
}

ConfigResult Config::parse(std::string_view toml, const std::filesystem::path &baseDir) {
    try {
        return adopt(config::ConfigLoader::loadFromString(toml, "<string>", baseDir), "<string>");
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrow(e, codes::kErrConfigParse, "<string>");
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

SceneConfig Config::scene() const {
    SceneConfig slice;
    slice.impl =
        std::make_shared<const SceneConfig::Impl>(SceneConfig::Impl{api::documentOf(*this)});
    return slice;
}

OutputConfig Config::output() const {
    OutputConfig slice;
    slice.impl =
        std::make_shared<const OutputConfig::Impl>(OutputConfig::Impl{api::documentOf(*this)});
    return slice;
}

bool Config::valid() const noexcept { return impl != nullptr; }

bool SceneConfig::valid() const noexcept { return impl != nullptr && impl->cfg != nullptr; }

bool OutputConfig::valid() const noexcept { return impl != nullptr && impl->cfg != nullptr; }

} // namespace nodehammer
