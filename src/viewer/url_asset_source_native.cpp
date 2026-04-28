#include <nodehammer/viewer/url_asset_source.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace nodehammer::viewer {

struct UrlAssetSource::Impl {
    LoadState state{LoadState::Idle};
    std::string error;
    std::filesystem::path config_path;
    std::filesystem::path input_path;
    std::vector<AssetProgress> entries;
    std::unordered_set<std::string> seen;
};

UrlAssetSource::UrlAssetSource() : impl_(std::make_unique<Impl>()) {}
UrlAssetSource::~UrlAssetSource() = default;

void UrlAssetSource::start(std::string config_url, std::string input_url,
                           std::string /*asset_base*/) {
    impl_->config_path = config_url;
    impl_->input_path = input_url;
    // Native: files already on disk; nothing to fetch. The async URL path
    // is emscripten-only by design.
    impl_->state = LoadState::Ready;
}

void UrlAssetSource::poll() {}

LoadState UrlAssetSource::state() const { return impl_->state; }

std::span<const AssetProgress> UrlAssetSource::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

const std::string &UrlAssetSource::errorMessage() const { return impl_->error; }
const std::filesystem::path &UrlAssetSource::configPath() const { return impl_->config_path; }
const std::filesystem::path &UrlAssetSource::inputPath() const { return impl_->input_path; }

} // namespace nodehammer::viewer
