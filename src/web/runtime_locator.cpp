#include "web/runtime_locator.hpp"

#include "diagnostic_codes.hpp"

#include <nodehammer/diagnostics.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <format>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nodehammer::web {
namespace {

#ifdef _WIN32
std::string getEnv(const char *name) {
    // GetEnvironmentVariableA rather than std::getenv, for the reasons
    // src/cli/pager.cpp gives: no deprecation warning, correct sizing, and
    // thread-safe.
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string buf(needed, '\0');
    DWORD written = GetEnvironmentVariableA(name, buf.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    buf.resize(written);
    return buf;
}
#else
std::string getEnv(const char *name) {
    const char *val = std::getenv(name);
    return (val != nullptr && val[0] != '\0') ? val : std::string{};
}
#endif

constexpr std::string_view kEnvVar = "NODEHAMMER_WEB_ASSETS";
constexpr std::string_view kStampFile = "nh_runtime.json";
constexpr std::string_view kShellFile = "viewer.html";

/// Why a candidate is not a runtime we can serve, or empty if it is.
///
/// The order of the checks is the order of the diagnoses, from "you pointed at
/// the wrong thing entirely" to "you pointed at a real runtime that disagrees",
/// so the message a person gets is the most specific one that is true.
std::string reject(const std::filesystem::path &dir, int *schemaOut, std::string *versionOut) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return "no such directory";
    }

    const std::filesystem::path stamp = dir / kStampFile;
    if (!std::filesystem::is_regular_file(stamp, ec)) {
        // Two very different mistakes, and the shell tells them apart: a
        // directory with a viewer.html is a runtime that predates the stamp (or
        // a hand-assembled one), which is worth saying, because "not a runtime"
        // would be wrong and would send the reader looking in the wrong place.
        if (std::filesystem::is_regular_file(dir / kShellFile, ec)) {
            return std::format("has {} but no {} — built before the runtime carried a stamp",
                               kShellFile, kStampFile);
        }
        return std::format("not a nodehammer web runtime (no {})", kStampFile);
    }

    nlohmann::json doc;
    {
        std::ifstream in(stamp, std::ios::binary);
        if (!in) {
            return std::format("{} cannot be opened", kStampFile);
        }
        doc = nlohmann::json::parse(in, nullptr, false);
    }
    if (doc.is_discarded()) {
        return std::format("{} is not valid JSON", kStampFile);
    }

    const auto schema = doc.find("schema");
    if (schema == doc.end() || !schema->is_number_integer()) {
        return std::format("{} carries no integer \"schema\"", kStampFile);
    }
    const auto version = doc.find("version");
    if (version == doc.end() || !version->is_string()) {
        return std::format("{} carries no \"version\" string", kStampFile);
    }

    const int found = schema->get<int>();
    if (found != compiledSchema()) {
        return std::format("built for schema {} ({}), but this library serves schema {}", found,
                           version->get<std::string>(), compiledSchema());
    }

    if (schemaOut != nullptr) {
        *schemaOut = found;
    }
    if (versionOut != nullptr) {
        *versionOut = version->get<std::string>();
    }
    return {};
}

} // namespace

std::string_view describe(RuntimeRung rung) noexcept {
    switch (rung) {
    case RuntimeRung::Explicit:
        return "--web-assets";
    case RuntimeRung::Environment:
        return kEnvVar;
    case RuntimeRung::InstallTree:
        return "install tree";
    }
    return "?";
}

int compiledSchema() noexcept { return NH_WEB_RUNTIME_SCHEMA; }

std::vector<RuntimeCandidate> walkLadder(const std::filesystem::path &explicitDir) {
    std::vector<RuntimeCandidate> ladder;

    auto consider = [&ladder](RuntimeRung rung, const std::filesystem::path &dir) {
        RuntimeCandidate c{rung, dir, {}};
        c.rejection = reject(dir, nullptr, nullptr);
        ladder.push_back(std::move(c));
        return ladder.back().rejection.empty();
    };

    if (!explicitDir.empty() && consider(RuntimeRung::Explicit, explicitDir)) {
        return ladder;
    }
    // A rung that was *stated* and did not answer stops the walk. Falling
    // through from a wrong --web-assets to whatever happens to be installed
    // would serve something other than what was asked for and say nothing.
    if (!ladder.empty()) {
        return ladder;
    }

    const std::string env = getEnv(std::string{kEnvVar}.c_str());
    if (!env.empty()) {
        consider(RuntimeRung::Environment, std::filesystem::path{env});
        return ladder;
    }

    // The only rung that is a guess. `<exe>/../share/nodehammer/web` is where a
    // native install tree carries a merged wasm one; its absence is the normal
    // state of a build from source, since the wasm is a separate Emscripten
    // build that a native configure cannot produce.
    if (const auto exe = executablePath()) {
        consider(RuntimeRung::InstallTree,
                 exe->parent_path().parent_path() / "share" / "nodehammer" / "web");
    } else {
        ladder.push_back(
            {RuntimeRung::InstallTree, {}, "this build cannot locate its own executable"});
    }
    return ladder;
}

RuntimeLocation locateRuntime(const std::filesystem::path &explicitDir) {
    const std::vector<RuntimeCandidate> ladder = walkLadder(explicitDir);

    if (!ladder.empty() && ladder.back().rejection.empty()) {
        const std::filesystem::path &dir = ladder.back().path;
        RuntimeLocation loc{dir, 0, {}};
        reject(dir, &loc.schema, &loc.version);
        return loc;
    }

    // Every rung, named, with the directory it looked at. A bare "not found"
    // here would read as a bug in the build the reader just made.
    std::string detail;
    for (const RuntimeCandidate &c : ladder) {
        detail += std::format("\n  {} [{}]: {}", c.path.empty() ? "(unavailable)" : c.path.string(),
                              describe(c.rung), c.rejection);
    }
    if (ladder.empty()) {
        detail = "\n  (no candidate: no --web-assets, no " + std::string{kEnvVar} +
                 ", and no executable path)";
    }

    // A schema disagreement is its own code: the remedy is to match the two
    // halves, not to go find a runtime, and a caller may want to say so
    // differently.
    const bool mismatch =
        ladder.size() == 1 && ladder.back().rejection.starts_with("built for schema ");

    throw Error{mismatch ? codes::kFatalWebRuntimeSchema : codes::kFatalWebRuntimeNotFound,
                std::format("no usable web viewer runtime; looked at:{}", detail),
                ladder.empty() ? std::string{} : ladder.back().path.string()};
}

} // namespace nodehammer::web
