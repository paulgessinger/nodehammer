#include <viewer/build_session.hpp>

#include <ir/fb/semantic/importer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <set>
#include <span>
#include <string>
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

bool isTomlKey(std::string_view key) { return endsWithCi(key, ".toml"); }

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

    /// Bytes-by-key collected during the walk. Each value is a
    /// `ByteBuffer` handle — refcounted, so the bytes survive a project
    /// mutation even if the backend drops its own reference.
    std::unordered_map<std::string, ByteBuffer> bytes_by_key;

    /// Keys to attempt to resolve next. Populated initially with the
    /// config + input keys; expanded as configs land and their includes
    /// are peeked.
    std::deque<std::string> work_queue;

    /// Keys we've already enqueued into `work_queue` (or finished). Used
    /// for cycle detection and to avoid re-enqueuing during repeated
    /// polls before the bytes land.
    std::set<std::string> seen;

    /// Generation of the project at the time of the last poll. Used to
    /// detect mutations between polls (e.g. user dropped a missing
    /// include) and re-walk.
    std::uint64_t last_project_generation{0};
    bool force_walk{false};

    /// Once all bytes are gathered, we run parse + import once and
    /// stash the result here for the App to consume.
    std::unique_ptr<BuildSessionInputs> inputs;

    void resetWalk() {
        bytes_by_key.clear();
        work_queue.clear();
        seen.clear();
        missing.clear();
        inputs.reset();
        if (config_key.empty() || geometry_key.empty()) {
            phase = BuildPhase::Idle;
        } else {
            phase = BuildPhase::Walking;
            work_queue.push_back(config_key);
            work_queue.push_back(geometry_key);
            seen.insert(config_key);
            seen.insert(geometry_key);
        }
    }
};

BuildSession::BuildSession() : impl_(std::make_unique<Impl>()) {}
BuildSession::~BuildSession() = default;

void BuildSession::setRootKeys(std::string config_key, std::string geometry_key) {
    impl_->config_key = std::move(config_key);
    impl_->geometry_key = std::move(geometry_key);
    impl_->resetWalk();
    impl_->force_walk = true;
}

void BuildSession::invalidate() {
    impl_->resetWalk();
    impl_->force_walk = true;
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

void BuildSession::poll(ProjectFs *project) {
    if (project == nullptr || impl_->config_key.empty() || impl_->geometry_key.empty()) {
        impl_->phase = BuildPhase::Idle;
        return;
    }

    const auto cur_gen = project->generation();
    const bool gen_changed = cur_gen != impl_->last_project_generation;
    impl_->last_project_generation = cur_gen;

    if (impl_->force_walk || gen_changed) {
        // Fresh walk. Throw away any prior partial state and start over.
        // (The Consumed → invalidate cycle uses this path: when a project
        // mutation lands, the App's setRootKeys / invalidate forces it.)
        impl_->resetWalk();
        impl_->force_walk = false;
    }

    if (impl_->phase != BuildPhase::Walking) {
        return;
    }

    auto enter_error = [&](std::string msg) {
        impl_->phase = BuildPhase::Error;
        pushError(std::move(msg));
    };

    // One-pass attempt to drain the work queue. Pending keys go to the
    // back; if any remain pending after a full pass, we yield. Missing/
    // Error short-circuit the walk.
    std::deque<std::string> retry;
    bool any_pending = false;
    while (!impl_->work_queue.empty()) {
        const auto key = std::move(impl_->work_queue.front());
        impl_->work_queue.pop_front();

        // Already resolved during this pass (e.g. via include expansion)?
        if (impl_->bytes_by_key.contains(key)) {
            continue;
        }

        auto result = project->resolve(key);
        switch (result.status) {
        case ResolveStatus::Ready: {
            // The ByteBuffer pins the bytes by refcount; they stay
            // valid even if the backend later replaces or drops its
            // own reference.
            auto buf = result.file.bytes;
            // Discover and enqueue nested includes for any TOML key.
            if (isTomlKey(key)) {
                auto rels = config::ConfigLoader::peekIncludesFromBytes(buf.span());
                for (const auto &rel : rels) {
                    auto abs = config::ConfigLoader::resolveIncludeKey(key, rel);
                    if (impl_->seen.insert(abs).second) {
                        impl_->work_queue.push_back(abs);
                    }
                }
            }
            impl_->bytes_by_key.emplace(key, std::move(buf));
            break;
        }
        case ResolveStatus::Pending:
            any_pending = true;
            retry.push_back(key);
            break;
        case ResolveStatus::Missing:
            impl_->missing.push_back(result.missing_key.empty() ? key : result.missing_key);
            // Keep the key on the retry list so a subsequent project
            // mutation (user drops the missing file) lets us pick it up
            // without restarting the whole walk.
            retry.push_back(key);
            break;
        case ResolveStatus::Error:
            enter_error(result.error.empty() ? ("resolve failed: " + key) : result.error);
            return;
        }
    }
    impl_->work_queue = std::move(retry);

    if (!impl_->missing.empty()) {
        impl_->phase = BuildPhase::WaitingForUser;
        return;
    }
    if (any_pending) {
        impl_->phase = BuildPhase::Walking;
        return;
    }

    // All bytes in hand. Parse the config (with the bytes map as the
    // synchronous fetcher) and import the geometry.
    auto cfg_it = impl_->bytes_by_key.find(impl_->config_key);
    auto in_it = impl_->bytes_by_key.find(impl_->geometry_key);
    if (cfg_it == impl_->bytes_by_key.end() || in_it == impl_->bytes_by_key.end()) {
        enter_error("internal: bytes missing for known root keys");
        return;
    }

    auto fetcher = [this](std::string_view k) -> std::optional<std::span<const std::byte>> {
        auto it = impl_->bytes_by_key.find(std::string{k});
        if (it == impl_->bytes_by_key.end()) {
            return std::nullopt;
        }
        return it->second.span();
    };

    auto cfg =
        config::ConfigLoader::parseAndMerge(cfg_it->second.span(), impl_->config_key, fetcher);
    for (const auto &d : cfg.diags.items()) {
        if (d.severity < diagnostics::DiagnosticSeverity::Error) {
            pushWarning(d.message);
        }
    }
    if (cfg.diags.hasErrors()) {
        std::string first = "config parse failed";
        for (const auto &d : cfg.diags.items()) {
            if (d.severity >= diagnostics::DiagnosticSeverity::Error) {
                first = d.message;
                break;
            }
        }
        enter_error(std::move(first));
        return;
    }

    auto imp = ir::FlatBufferImporter::importFromBytes(impl_->geometry_key, in_it->second.span());
    for (const auto &d : imp.diags.items()) {
        if (d.severity < diagnostics::DiagnosticSeverity::Error) {
            pushWarning(d.message);
        }
    }
    if (imp.diags.hasErrors()) {
        std::string first = "geometry import failed";
        for (const auto &d : imp.diags.items()) {
            if (d.severity >= diagnostics::DiagnosticSeverity::Error) {
                first = d.message;
                break;
            }
        }
        enter_error(std::move(first));
        return;
    }

    auto inputs = std::make_unique<BuildSessionInputs>();
    inputs->config = std::move(cfg);
    inputs->import = std::move(imp);
    inputs->config_key = impl_->config_key;
    inputs->geometry_key = impl_->geometry_key;
    inputs->input_hash = hashInputs(impl_->bytes_by_key);
    impl_->inputs = std::move(inputs);
    impl_->phase = BuildPhase::ResolvedReady;
}

} // namespace nodehammer::viewer
