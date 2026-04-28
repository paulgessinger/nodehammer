#include <nodehammer/viewer/bag_project_fs.hpp>

#include <nodehammer/ir/semantic/importer.hpp>

#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

namespace {

// Lowercase ASCII compare for extension recognition. A user-dropped FOO.TOML
// is still a config; std::tolower depends on locale, so a homemade variant
// keeps the comparison predictable.
char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool extEquals(const std::filesystem::path &p, std::string_view want) {
    const auto ext = p.extension().string();
    if (ext.size() != want.size() + 1 || ext[0] != '.') {
        return false;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        if (asciiLower(ext[i + 1]) != want[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

struct BagProjectFs::Impl {
    ProjectFsStatus status{ProjectFsStatus::Idle};
    std::string error;
    std::filesystem::path config_path;
    std::filesystem::path input_path;
    std::vector<ProjectProgress> entries;
    std::string last_unrecognised;
    // Cached "what's missing" strings, recomputed alongside `status`.
    // Stored so `waitingFor()` can return a span without allocating.
    std::vector<std::string> waiting;
    // Soft warnings; e.g. last-write-wins notes on basename collisions.
    std::vector<std::string> warning_msgs;

    // Cached importer registry for extension probing. Constructing it once
    // avoids re-registering every importer on each ingested file.
    ImporterRegistry registry{ImporterRegistry::makeDefault()};

    bool hasConfig() const { return !config_path.empty(); }
    bool hasInput() const { return !input_path.empty(); }

    void recomputeStatus() {
        if (status == ProjectFsStatus::Error) {
            return;
        }
        if (hasConfig() && hasInput()) {
            status = ProjectFsStatus::Ready;
        } else if (hasConfig() || hasInput()) {
            status = ProjectFsStatus::Fetching;
        } else {
            status = ProjectFsStatus::Idle;
        }
        waiting.clear();
        if (!hasConfig()) {
            waiting.emplace_back("a .toml config");
        }
        if (!hasInput()) {
            waiting.emplace_back("a geometry file (.nhb.zst, .gdml, .gltf, …)");
        }
    }

    void recordEntry(const std::filesystem::path &path) {
        // bytes_total / bytes_done left at 0 — local files are effectively
        // instant; the UI just shows the filename. If we ever want to read
        // them through a streaming layer we can fill these in.
        ProjectProgress p;
        p.url = path.filename().string();
        p.done = true;
        p.bytes_total = 0;
        p.bytes_done = 0;
        entries.push_back(std::move(p));
    }
};

BagProjectFs::BagProjectFs() : impl_(std::make_unique<Impl>()) {}
BagProjectFs::~BagProjectFs() = default;

void BagProjectFs::poll() {}

ProjectFsStatus BagProjectFs::status() const { return impl_->status; }

std::span<const ProjectProgress> BagProjectFs::progress() const {
    return {impl_->entries.data(), impl_->entries.size()};
}

const std::string &BagProjectFs::errorMessage() const { return impl_->error; }
const std::filesystem::path &BagProjectFs::rootConfigPath() const { return impl_->config_path; }
const std::filesystem::path &BagProjectFs::rootInputPath() const { return impl_->input_path; }

std::span<const std::string> BagProjectFs::waitingFor() const {
    return {impl_->waiting.data(), impl_->waiting.size()};
}

std::span<const std::string> BagProjectFs::warnings() const {
    return {impl_->warning_msgs.data(), impl_->warning_msgs.size()};
}

const std::string &BagProjectFs::unrecognised() const { return impl_->last_unrecognised; }

void BagProjectFs::addBytes(std::string_view filename, std::span<const std::byte> bytes) {
    if (filename.empty()) {
        return;
    }
    // Stage uploads under the OS temp dir so the rest of the build pipeline
    // (which expects paths) can read them back like any other file. Under
    // Emscripten temp_directory_path resolves to "/tmp" in MEMFS, so this
    // works on both targets without ifdefs.
    std::error_code ec;
    const auto staging = std::filesystem::temp_directory_path(ec) / "nodehammer-uploads";
    if (ec) {
        impl_->error = "failed to resolve temp dir for upload staging: " + ec.message();
        impl_->status = ProjectFsStatus::Error;
        return;
    }
    std::filesystem::create_directories(staging, ec);
    const auto path = staging / std::filesystem::path{filename}.filename();
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        impl_->error = "failed to stage upload at " + path.string();
        impl_->status = ProjectFsStatus::Error;
        return;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        impl_->error = "short write while staging upload at " + path.string();
        impl_->status = ProjectFsStatus::Error;
        return;
    }
    out.close();
    // Now that the bytes live at a real path, run the same role-recognition
    // path as a native drop / NFD pick — single source of truth for which
    // extensions land in which slot.
    addPath(path);
}

void BagProjectFs::addPath(const std::filesystem::path &path) {
    if (path.empty()) {
        return;
    }

    // 1) Config: any .toml file fills the config slot.
    if (extEquals(path, "toml")) {
        if (!impl_->config_path.empty() && impl_->config_path != path) {
            impl_->warning_msgs.emplace_back("replaced " + impl_->config_path.filename().string() +
                                             " with " + path.string());
        }
        impl_->config_path = path;
        impl_->last_unrecognised.clear();
        impl_->recordEntry(path);
        impl_->recomputeStatus();
        return;
    }

    // 2) Geometry: ask the importer registry whether any registered importer
    //    claims this extension. Keeps the bag format-agnostic — adding a new
    //    geometry format is purely an importer change.
    if (impl_->registry.resolve(path, {}) != nullptr) {
        if (!impl_->input_path.empty() && impl_->input_path != path) {
            impl_->warning_msgs.emplace_back("replaced " + impl_->input_path.filename().string() +
                                             " with " + path.string());
        }
        impl_->input_path = path;
        impl_->last_unrecognised.clear();
        impl_->recordEntry(path);
        impl_->recomputeStatus();
        return;
    }

    // 3) Unrecognised: stash the filename so the UI can hint at it.
    impl_->last_unrecognised = path.filename().string();
}

} // namespace nodehammer::viewer
