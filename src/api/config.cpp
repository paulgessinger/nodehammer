#include <api/handles.hpp>

#include <config/config_loader.hpp>
#include <config/config_validator.hpp>
#include <detail/file_io.hpp>
#include <diagnostic_codes.hpp>

#if NH_WITH_LUA
#include <lua/lua_config.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <string>
#include <utility>

namespace nodehammer {
namespace {

using api::Access;

[[nodiscard]] ConfigResult failure(std::string_view code, std::string_view message,
                                   std::string_view context = {}) {
    return ConfigResult{Config{}, Access::error(code, message, context)};
}

/// Adopt an internal load, then validate — the one thing these wrappers add
/// beyond dispatch and slicing.
///
/// Validation is skipped after a failed load, matching `convert`: a load error
/// leaves a default-constructed AST, and validating that would answer a
/// question about a document nobody wrote. Validation *diagnostics* land in the
/// same list as the load's, which is what makes `build` free of a validation
/// step of its own (#41 §8).
[[nodiscard]] ConfigResult adopt(config::ConfigResult loaded) {
    std::vector<Diagnostic> diags = Access::items(loaded.diags);
    if (loaded.diags.hasErrors()) {
        return ConfigResult{Config{}, Access::seal(std::move(diags))};
    }
    Access::appendTo(diags, config::ConfigValidator::validate(loaded.config));
    return ConfigResult{Access::wrap(std::move(loaded.config)), Access::seal(std::move(diags))};
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
        return adopt(config::ConfigLoader::loadFromFile(path));
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
        return failure(codes::kErrImportFileNotFound,
                       std::format("could not read config file: {}", e.what()), path.string());
    }
    try {
        return adopt(lua::evalLuaConfig(source, canonical.string(), canonical.parent_path()));
    } catch (const std::exception &e) {
        return failure(codes::kErrConfigParse, e.what(), path.string());
    }
#else
    return failure(codes::kErrApiBackendMissing,
                   "this build has no Lua config front end; see formats()", path.string());
#endif
}

ConfigResult Config::parse(std::string_view toml, const std::filesystem::path &baseDir) {
    try {
        return adopt(config::ConfigLoader::loadFromString(toml, "<string>", baseDir));
    } catch (const std::exception &e) {
        return failure(codes::kErrConfigParse, e.what());
    }
}

std::vector<std::string> Config::formats() {
    std::vector<std::string> out{"toml"};
#if NH_WITH_LUA
    out.emplace_back("lua");
#endif
    return out;
}

SceneConfig Config::scene() const { return Access::sceneSlice(Access::documentOf(impl_)); }

OutputConfig Config::output() const { return Access::outputSlice(Access::documentOf(impl_)); }

bool Config::valid() const noexcept { return impl_ != nullptr; }

bool SceneConfig::valid() const noexcept { return impl_ != nullptr && impl_->cfg != nullptr; }

bool OutputConfig::valid() const noexcept { return impl_ != nullptr && impl_->cfg != nullptr; }

} // namespace nodehammer
