#pragma once

#include <config/config_loader.hpp> // config::ConfigResult

#include <filesystem>
#include <string_view>

namespace nodehammer::lua {

/// Evaluate a Lua config script into an NHConfig.
///
/// The script drives a small global-function builder DSL — `config`, `export`,
/// `material`, `keep`, `drop`, `rule`, `defaults`, plus `include` / `use` for
/// composition — whose calls assemble the returned config. See
/// docs/config-scripting-lua.md for the language design.
///
/// `sourceName` is the chunk name used in Lua error messages (typically the
/// script path). `baseDir` roots the relative paths passed to `include()` /
/// `use()`; nested includes resolve relative to the including file.
///
/// A *collecting* face, like `ConfigLoader::collectFromString`: Lua errors
/// (syntax or runtime), unparseable predicate expressions and DSL misuse are
/// reported through `diags` rather than thrown, because naming every problem in
/// a script is the job. The config may be only partially built when they are
/// present. A caller that promised a config rather than a report runs
/// `diagnostics::throwIfErrors` over the result — which is what `Config::read`
/// does (docs/error-model.md).
///
/// This is the Option-A core of the scripting front-end: the `config-lua` CLI
/// command wraps it with `configToToml`. The same entry point can later back an
/// embedded `ConfigLoader::loadFromLua` (Option B) unchanged.
[[nodiscard]] config::ConfigResult evalLuaConfig(std::string_view src, std::string_view sourceName,
                                                 const std::filesystem::path &baseDir);

} // namespace nodehammer::lua
