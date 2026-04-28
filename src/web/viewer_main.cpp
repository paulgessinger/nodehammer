// Browser entry point for the wasm viewer.
//
// The native build still goes through CLI11 + the `viewer` subcommand, but
// in the browser the only meaningful entry path is "open a viewer with
// these URLs and these flags". So instead of shipping CLI11 + every
// subcommand registration TU into the wasm bundle, we expose a single
// JSON-shaped C export and let the JS shell call it after runtime init.
//
// Lifecycle (matches the CLI's `viewer` subcommand on web):
//   1. JS shell calls `_nh_viewer_start(opts_json)` once.
//   2. Options JSON populates a viewer::Config + (optionally) an
//      initial_camera.
//   3. If both `config` and `input` URL-style paths are supplied, a
//      UrlProjectFs is mounted on the App and fetches them via
//      emscripten_fetch. Otherwise the App keeps its eagerly-allocated
//      BagProjectFs; drag-drop or the file picker push files into it.
//   4. `App::run()` registers the sokol main loop with emscripten and
//      unwinds; subsequent frame callbacks fire from the event queue.
//
// Returns 0 on success, non-zero if the JSON could not be parsed.

#include <nlohmann/json.hpp>

#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/config.hpp>
#include <nodehammer/viewer/url_project_fs.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

// Top-level option keys recognised by nh_viewer_start. Keep this list in
// sync with the readField calls below — the warnUnknownKeys check uses it
// to flag silent typos in JSON keys (the boundary's main weakness vs. a
// strongly-typed binding like embind).
constexpr std::array<std::string_view, 17> kKnownOptionKeys = {
    "title",     "width",      "height",   "vsync",          "cullBack", "pauseWhenUnfocused",
    "autoOrbit", "orbitSpeed", "angleCut", "shaderAngleCut", "cutStart", "cutEnd",
    "pbr",       "input",      "config",   "assetBase",      "camera",
};

constexpr std::array<std::string_view, 6> kKnownCameraKeys = {
    "targetX", "targetY", "targetZ", "distance", "yawDeg", "pitchDeg",
};

template <std::size_t N>
bool isKnownKey(std::string_view key, const std::array<std::string_view, N> &set) {
    for (std::string_view k : set) {
        if (k == key) {
            return true;
        }
    }
    return false;
}

template <std::size_t N>
void warnUnknownKeys(const nlohmann::json &j, const std::array<std::string_view, N> &known,
                     const char *scope) {
    if (!j.is_object()) {
        return;
    }
    for (const auto &item : j.items()) {
        if (!isKnownKey(item.key(), known)) {
            std::fprintf(stderr,
                         "nh_viewer_start: ignoring unknown %s key \"%s\" — typo or stale "
                         "JS shell?\n",
                         scope, item.key().c_str());
        }
    }
}

template <typename T> void readField(const nlohmann::json &j, const char *key, T &out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        out = it->get<T>();
    }
}

std::optional<nodehammer::viewer::Camera> parseCamera(const nlohmann::json &j) {
    auto it = j.find("camera");
    if (it == j.end() || !it->is_object()) {
        return std::nullopt;
    }
    const auto &c = *it;
    warnUnknownKeys(c, kKnownCameraKeys, "camera");
    // All six fields are required to restore a camera — partial state is
    // ambiguous, so fall back to defaults if any is missing. Matches the
    // CLI's `--camera-*` mutual-requirement check.
    if (!c.contains("targetX") || !c.contains("targetY") || !c.contains("targetZ") ||
        !c.contains("distance") || !c.contains("yawDeg") || !c.contains("pitchDeg")) {
        return std::nullopt;
    }
    nodehammer::viewer::Camera cam;
    cam.target.x = c["targetX"].get<float>();
    cam.target.y = c["targetY"].get<float>();
    cam.target.z = c["targetZ"].get<float>();
    cam.distance = c["distance"].get<float>();
    if (cam.distance <= 0.f) {
        return std::nullopt;
    }
    cam.yaw = glm::radians(c["yawDeg"].get<float>());
    cam.pitch = glm::radians(c["pitchDeg"].get<float>());
    return cam;
}

} // namespace

extern "C" {

// Called by the JS shell after the runtime is initialized. `opts_json` is
// a JSON object whose keys mirror the native CLI's `viewer` flags. Returns
// 0 on success; 1 if the JSON could not be parsed.
__attribute__((used)) int nh_viewer_start(const char *opts_json) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(opts_json != nullptr ? opts_json : "{}");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "nh_viewer_start: JSON parse error: %s\n", e.what());
        return 1;
    }

    warnUnknownKeys(j, kKnownOptionKeys, "option");

    nodehammer::viewer::Config cfg;
    readField(j, "title", cfg.title);
    readField(j, "width", cfg.width);
    readField(j, "height", cfg.height);
    readField(j, "vsync", cfg.vsync);
    readField(j, "cullBack", cfg.cull_back);
    readField(j, "pauseWhenUnfocused", cfg.pause_when_unfocused);
    readField(j, "autoOrbit", cfg.auto_orbit);
    readField(j, "orbitSpeed", cfg.auto_orbit_speed_deg);
    readField(j, "angleCut", cfg.angle_cut);
    readField(j, "shaderAngleCut", cfg.shader_angle_cut);
    readField(j, "cutStart", cfg.angle_cut_start_deg);
    readField(j, "cutEnd", cfg.angle_cut_end_deg);
    readField(j, "pbr", cfg.enable_pbr);
    cfg.initial_camera = parseCamera(j);

    std::string inputPath, configPath, assetBase;
    readField(j, "input", inputPath);
    readField(j, "config", configPath);
    readField(j, "assetBase", assetBase);

    nodehammer::viewer::App::Handle application(cfg);

    if (!inputPath.empty() && !configPath.empty()) {
        auto loader = std::make_unique<nodehammer::viewer::UrlProjectFs>();
        loader->start(configPath, inputPath, assetBase);
        application->setProject(std::move(loader));
    }
    // Otherwise the App keeps its eagerly-allocated BagProjectFs — the
    // user's first drop or Open-files gesture pushes into it directly.

    application->run();
    return 0;
}

} // extern "C"

// Empty main() so emscripten's INVOKE_RUN path completes cleanly without
// doing anything. The viewer is started later via nh_viewer_start; this
// keeps the runtime alive (paired with EXIT_RUNTIME=0).
int main(int /*argc*/, char ** /*argv*/) { return 0; }
