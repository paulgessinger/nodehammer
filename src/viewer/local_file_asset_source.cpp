#include <nodehammer/viewer/local_file_asset_source.hpp>

#include <nodehammer/ir/semantic/importer.hpp>

#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

// Lowercase ASCII compare for extension recognition. Real path extension
// comparison should be case-insensitive (a user-dropped FOO.TOML is still
// a config). Cheap homemade variant — std::tolower depends on locale.
char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ext_equals(const std::filesystem::path &p, std::string_view want) {
    const auto ext = p.extension().string();
    if (ext.size() != want.size() + 1 || ext[0] != '.') {
        return false;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        if (ascii_lower(ext[i + 1]) != want[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

struct LocalFileAssetSource::Impl {
    LoadState state{LoadState::Idle};
    std::string error;
    std::filesystem::path config_path;
    std::filesystem::path input_path;
    std::vector<AssetProgress> entries;
    std::string last_unrecognised;

    // Cached importer registry for extension probing. Constructing it once
    // avoids re-registering every importer on each dropped file.
    ImporterRegistry registry{ImporterRegistry::makeDefault()};

    bool has_config() const { return !config_path.empty(); }
    bool has_input() const { return !input_path.empty(); }

    void recompute_state() {
        if (state == LoadState::Error) {
            return;
        }
        if (has_config() && has_input()) {
            state = LoadState::Ready;
        } else if (has_config() || has_input()) {
            state = LoadState::Fetching; // "still accumulating"
        } else {
            state = LoadState::Idle;
        }
    }

    void record_entry(const std::filesystem::path &path) {
        // bytes_total / bytes_done left at 0 — local files are effectively
        // instant; the UI just shows the filename. If we ever want to read
        // them through a streaming layer we can fill these in.
        AssetProgress p;
        p.url = path.filename().string();
        p.done = true;
        p.bytes_total = 0;
        p.bytes_done = 0;
        entries.push_back(std::move(p));
    }
};

LocalFileAssetSource::LocalFileAssetSource() : impl_(std::make_unique<Impl>()) {}
LocalFileAssetSource::~LocalFileAssetSource() = default;

bool LocalFileAssetSource::needs_config() const { return !impl_->has_config(); }
bool LocalFileAssetSource::needs_input() const { return !impl_->has_input(); }
const std::string &LocalFileAssetSource::last_unrecognised() const {
    return impl_->last_unrecognised;
}

void LocalFileAssetSource::poll() {}

LoadState LocalFileAssetSource::state() const { return impl_->state; }

std::span<const AssetProgress> LocalFileAssetSource::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

const std::string &LocalFileAssetSource::error_message() const { return impl_->error; }
const std::filesystem::path &LocalFileAssetSource::config_path() const {
    return impl_->config_path;
}
const std::filesystem::path &LocalFileAssetSource::input_path() const { return impl_->input_path; }

void LocalFileAssetSource::ingest_local_file(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }

    // 1) Config: any .toml file fills the config slot.
    if (ext_equals(path, "toml")) {
        impl_->config_path = path;
        impl_->last_unrecognised.clear();
        impl_->record_entry(path);
        impl_->recompute_state();
        return;
    }

    // 2) Geometry: ask the importer registry whether any registered importer
    //    claims this extension. Keeps the source format-agnostic — adding
    //    a new geometry format is purely an importer change.
    if (impl_->registry.resolve(path, {}) != nullptr) {
        impl_->input_path = path;
        impl_->last_unrecognised.clear();
        impl_->record_entry(path);
        impl_->recompute_state();
        return;
    }

    // 3) Unrecognised: stash the filename so the UI can hint at it.
    impl_->last_unrecognised = path.filename().string();
}

} // namespace nodehammer::viewer
