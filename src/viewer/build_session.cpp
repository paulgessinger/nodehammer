#include <viewer/build_session.hpp>

#include <ir/fb/semantic/importer.hpp>
#include <lua/lua_config.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

bool endsWithCi(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// The only extension test the session still needs. There is no `isTomlKey`
// any more: TOML used to be the format whose includes could be *peeked* ahead
// of parsing, and nothing peeks now.
bool isLuaKey(std::string_view key) { return endsWithCi(key, ".lua"); }

/// FNV-1a 64-bit over an arbitrary byte range.
void fnv1a(std::uint64_t &h, std::span<const std::byte> bytes) {
    for (const std::byte b : bytes) {
        h ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(b));
        h *= 0x00000100000001B3ULL;
    }
}

/// Order-independent, content-addressed hash of the resolved input byte set.
/// Sorting the keys makes it stable regardless of resolve order, and folding the
/// key in (with its length) keeps two files from aliasing across a boundary.
/// The one thing that talks to the project during a refresh.
///
/// It exists so that "what could not be supplied" is *recorded* rather than
/// inferred afterwards from diagnostics. `ProjectFs` already separates "the
/// project does not have this" from "the backend broke", and only the first is
/// something a user can fix by adding a file — a distinction that would be lost
/// if the session went looking for `kFatalImportFileNotFound` in a thrown
/// exception, since the loader raises that code for its own reasons too.
///
/// It doubles as the config loader's `IncludeFetcher`, so the roots and the
/// include closure come through one door and land in one map — which is then
/// exactly what `input_hash` is taken over.
class ProjectFetch {
  public:
    explicit ProjectFetch(const ProjectFs &project) : project_(&project) {}

    /// Resolve a key once, remembering the outcome. Repeats are served from
    /// `pulled`, so a diamond include neither re-reads nor double-reports.
    std::optional<std::span<const std::byte>> operator()(std::string_view key) {
        const std::string k{key};
        if (const auto it = pulled.find(k); it != pulled.end()) {
            return it->second.span();
        }
        if (unresolved_.contains(k)) {
            return std::nullopt;
        }
        auto r = project_->resolve(k);
        switch (r.status) {
        case ResolveStatus::Ready: {
            auto [ins, _] = pulled.emplace(k, std::move(r.file.bytes));
            return ins->second.span();
        }
        case ResolveStatus::Missing:
            unresolved_.insert(k);
            missing.push_back(r.missing_key.empty() ? k : r.missing_key);
            return std::nullopt;
        case ResolveStatus::Error:
            unresolved_.insert(k);
            failed.push_back(k);
            if (failure.empty()) {
                failure = r.error.empty() ? ("resolve failed: " + k) : r.error;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    /// A view of this fetcher for the config loader. Valid only for the
    /// refresh that owns it, which is the only place it is handed out.
    config::IncludeFetcher asIncludeFetcher() {
        return [this](std::string_view key) { return (*this)(key); };
    }

    /// Every key that resolved, by key. Node-based, so spans handed out earlier
    /// stay valid as later ones are inserted.
    std::unordered_map<std::string, ByteBuffer> pulled;
    std::vector<std::string> missing; ///< the user can fix these by adding a file
    std::vector<std::string> failed;  ///< the backend broke; the user cannot
    std::string failure;              ///< first hard failure's message

  private:
    const ProjectFs *project_;
    std::set<std::string> unresolved_;
};

/// A resolved buffer as text, for the front ends that take a string.
std::string_view asText(std::span<const std::byte> bytes) {
    return std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

std::uint64_t hashInputs(const std::unordered_map<std::string, ByteBuffer> &bytes_by_key) {
    std::vector<const std::string *> keys;
    keys.reserve(bytes_by_key.size());
    for (const auto &[k, _] : bytes_by_key) {
        keys.push_back(&k);
    }
    std::sort(keys.begin(), keys.end(),
              [](const std::string *a, const std::string *b) { return *a < *b; });

    std::uint64_t h = 0xcbf29ce484222325ULL; // FNV offset basis
    for (const std::string *k : keys) {
        const auto klen = static_cast<std::uint64_t>(k->size());
        fnv1a(h, std::as_bytes(std::span{&klen, 1}));
        fnv1a(h, std::as_bytes(std::span{k->data(), k->size()}));
        fnv1a(h, bytes_by_key.at(*k).span());
    }
    return h;
}

} // namespace

struct BuildSession::Impl {
    std::string config_key;
    std::string geometry_key;
    BuildPhase phase{BuildPhase::Idle};
    std::vector<std::string> missing;

    /// Generation of the project at the time of the last refresh. Used to
    /// detect mutations between refreshes (e.g. the user dropped a missing
    /// include) and re-derive.
    std::uint64_t last_project_generation{0};
    bool force_refresh{false};

    /// The result of the last successful refresh, stashed for the App to
    /// consume. No byte cache sits beside it: each refresh reads what it
    /// needs from the project and hands the collected bytes to the hash,
    /// so there is nothing to keep coherent between refreshes.
    std::unique_ptr<BuildSessionInputs> inputs;

    void markStale() {
        missing.clear();
        inputs.reset();
        phase = (config_key.empty() || geometry_key.empty()) ? BuildPhase::Idle : BuildPhase::Stale;
    }
};

BuildSession::BuildSession() : impl_(std::make_unique<Impl>()) {}
BuildSession::~BuildSession() = default;

void BuildSession::setRootKeys(std::string config_key, std::string geometry_key) {
    impl_->config_key = std::move(config_key);
    impl_->geometry_key = std::move(geometry_key);
    impl_->markStale();
    impl_->force_refresh = true;
}

void BuildSession::invalidate() {
    impl_->markStale();
    impl_->force_refresh = true;
}

BuildPhase BuildSession::phase() const { return impl_->phase; }

std::span<const std::string> BuildSession::missing() const {
    return {impl_->missing.data(), impl_->missing.size()};
}

std::unique_ptr<BuildSessionInputs> BuildSession::takeInputs() {
    if (impl_->phase != BuildPhase::ResolvedReady || !impl_->inputs) {
        return nullptr;
    }
    auto out = std::move(impl_->inputs);
    impl_->phase = BuildPhase::Consumed;
    return out;
}

const std::string &BuildSession::rootConfigKey() const { return impl_->config_key; }
const std::string &BuildSession::rootGeometryKey() const { return impl_->geometry_key; }

void BuildSession::refresh(ProjectFs &project) {
    if (impl_->config_key.empty() || impl_->geometry_key.empty()) {
        impl_->phase = BuildPhase::Idle;
        return;
    }

    const auto cur_gen = project.generation();
    const bool gen_changed = cur_gen != impl_->last_project_generation;
    impl_->last_project_generation = cur_gen;

    if (impl_->force_refresh || gen_changed) {
        impl_->markStale();
        impl_->force_refresh = false;
    }

    if (impl_->phase != BuildPhase::Stale) {
        return;
    }

    auto enter_error = [&](std::string msg) {
        impl_->phase = BuildPhase::Error;
        pushError(std::move(msg));
    };
    auto wait_for = [&](std::vector<std::string> keys) {
        impl_->missing = std::move(keys);
        impl_->phase = BuildPhase::WaitingForUser;
    };

    ProjectFetch fetch{project};

    // The two roots. Not a walk — there is nothing to expand here, because a
    // config's includes are found by the fetcher while parsing and the geometry
    // is a single self-contained blob.
    const auto cfg_bytes = fetch(impl_->config_key);
    const auto geo_bytes = fetch(impl_->geometry_key);
    if (!fetch.failed.empty()) {
        enter_error(fetch.failure);
        return;
    }
    // A missing *geometry* is no reason to stop here. Includes are discovered by
    // parsing now, so returning before the parse would name the geometry alone
    // and leave the fragments behind it for the next refresh — one round trip
    // per layer, where the walk this replaced named them together. Only an
    // absent config forces the early exit, because there is nothing to parse;
    // anything still missing rides along and is reported after the parse.
    if (!cfg_bytes) {
        wait_for(std::move(fetch.missing));
        return;
    }

    // Parse the config, resolving its includes through the same fetcher. Which
    // front end runs is the extension's business; both take a root key and a
    // fetcher, and both report a missing include by asking the fetcher for it
    // and being told no.
    config::ConfigResult cfg;
    std::string parse_failure;
    try {
        if (isLuaKey(impl_->config_key)) {
            cfg =
                lua::evalLuaConfig(asText(*cfg_bytes), impl_->config_key, fetch.asIncludeFetcher());
            // The Lua face collects rather than throws, because naming every
            // problem in a script is its job. Here we wanted a config.
            diagnostics::throwIfErrors(cfg.diags, impl_->config_key);
        } else {
            cfg = config::ConfigLoader::parseAndMerge(*cfg_bytes, impl_->config_key,
                                                      fetch.asIncludeFetcher());
        }
    } catch (const Error &e) {
        for (const auto &d : e.observed()) {
            if (d.severity < diagnostics::Severity::Error) {
                pushWarning(d.message);
            }
        }
        parse_failure = e.what();
    }

    // What the fetcher could not supply outranks the parse failure, because a
    // missing include is *why* the parse failed. Telling someone "config parse
    // failed" when they simply have not added `materials.toml` yet describes the
    // symptom and hides the fix.
    if (!fetch.failed.empty()) {
        enter_error(fetch.failure);
        return;
    }
    if (!fetch.missing.empty()) {
        wait_for(std::move(fetch.missing));
        return;
    }
    if (!parse_failure.empty()) {
        enter_error(std::move(parse_failure));
        return;
    }
    for (const auto &d : cfg.diags.items()) {
        pushWarning(d.message);
    }

    ir::ImportResult imp;
    try {
        imp = ir::FlatBufferImporter::importFromBytes(impl_->geometry_key, *geo_bytes);
    } catch (const Error &e) {
        enter_error(e.what());
        return;
    }
    for (const auto &d : imp.diags.items()) {
        pushWarning(d.message);
    }

    auto inputs = std::make_unique<BuildSessionInputs>();
    inputs->config = std::move(cfg);
    inputs->import = std::move(imp);
    inputs->config_key = impl_->config_key;
    inputs->geometry_key = impl_->geometry_key;
    // Everything the refresh read, which is exactly the roots plus the include
    // closure the config actually reached for.
    inputs->input_hash = hashInputs(fetch.pulled);
    impl_->inputs = std::move(inputs);
    impl_->phase = BuildPhase::ResolvedReady;
}

} // namespace nodehammer::viewer
