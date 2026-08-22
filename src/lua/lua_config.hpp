#pragma once

#include <config/config_loader.hpp> // config::ConfigResult, config::IncludeFetcher

#include <string_view>

namespace nodehammer::lua {

/// Evaluate a Lua config script into an NHConfig.
///
/// The script drives a small global-function builder DSL — `config`, `export`,
/// `material`, `keep`, `drop`, `rule`, `defaults`, plus `include` / `use` for
/// composition — whose calls assemble the returned config. See
/// docs/config-scripting-lua.md for the language design.
///
/// `rootKey` names the script — it is the chunk name in Lua error messages, and
/// its parent directory roots `include()` / `use()`, exactly as the root key
/// roots `ConfigLoader::parseAndMerge`'s include tree. Nested includes resolve
/// against the key of the chunk that asked for them, via the same
/// `ConfigLoader::resolveIncludeKey`, so both front ends compute the same key
/// for the same include and neither owns a second notion of "where".
///
/// `fetcher` serves those keys. A script reads nothing else: the sandbox opens
/// no `io` and no `package`, so this is the front end's *entire* contact with
/// the outside, and what it can reach is the caller's decision rather than the
/// process's. `ConfigLoader::filesystemFetcher()` is the on-disk answer;
/// `BuildSession` serves a project's bytes instead, which is what lets a `.lua`
/// live inside a `.nhproj` where there is no filesystem to resolve against.
///
/// Keys that resolve outside the root's directory are refused before the fetcher
/// sees them, so a fetcher that *could* serve them — the filesystem one — still
/// does not. A script may be untrusted (design doc §9); its reach must not
/// depend on which fetcher it happened to be given.
///
/// A *collecting* face, like `ConfigLoader::collectFromString`: Lua errors
/// (syntax or runtime), unparseable predicate expressions and DSL misuse are
/// reported through `diags` rather than thrown, because naming every problem in
/// a script is the job. The config may be only partially built when they are
/// present. A caller that promised a config rather than a report runs
/// `diagnostics::throwIfErrors` over the result — which is what `Config::read`
/// does (docs/error-model.md).
///
/// This is the Option-A core of the scripting front-end: the `config flatten` CLI
/// command wraps it with `configToToml`. The same entry point can later back an
/// embedded `ConfigLoader::loadFromLua` (Option B) unchanged.
[[nodiscard]] config::ConfigResult evalLuaConfig(std::string_view src, std::string_view rootKey,
                                                 config::IncludeFetcher fetcher);

} // namespace nodehammer::lua
