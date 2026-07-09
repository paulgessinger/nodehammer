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
//   3. If an `archive` URL is supplied (viewer mode — the sidecar names a
//      `.nhproj`), its bytes are fetched via emscripten_fetch and opened as a
//      content-locked, provenance-Remote project. Otherwise the App keeps its
//      empty working set (application mode) and kicks an IndexedDB restore;
//      drag-drop / the file picker / Open-archive push into it.
//   4. `App::run()` registers the sokol main loop with emscripten and
//      unwinds; subsequent frame callbacks fire from the event queue.
//
// Returns 0 on success, non-zero if the JSON could not be parsed.

#include <nlohmann/json.hpp>

#include <nodehammer/viewer/app.hpp>
#include <nodehammer/viewer/config.hpp>

#include <emscripten/fetch.h>
#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

// Top-level option keys recognised by nh_viewer_start. Keep this list in
// sync with the readField calls below — the warnUnknownKeys check uses it
// to flag silent typos in JSON keys (the boundary's main weakness vs. a
// strongly-typed binding like embind).
constexpr std::array<std::string_view, 17> kKnownOptionKeys = {
    "title",     "width",      "height",   "vsync",          "cull",       "pauseWhenUnfocused",
    "autoOrbit", "orbitSpeed", "angleCut", "shaderAngleCut", "booleanCut", "cutStart",
    "cutEnd",    "pbr",        "archive",  "lock",           "camera",
};

constexpr std::array<std::string_view, 7> kKnownCameraKeys = {
    "targetX", "targetY", "targetZ", "distance", "yawDeg", "pitchDeg", "projection",
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
                         "nh_viewer_start: ignoring unknown %s key \"%s\" -- typo or stale "
                         "JS shell?\n",
                         scope, item.key().c_str());
        }
    }
}

template <typename T> bool readField(const nlohmann::json &j, const char *key, T &out) {
    if (auto it = j.find(key); it != j.end() && !it->is_null()) {
        out = it->get<T>();
        return true;
    }
    return false;
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
    if (auto proj = c.find("projection"); proj != c.end() && proj->is_string()) {
        const auto value = proj->get<std::string>();
        if (value == "orthographic" || value == "ortho") {
            cam.projection = nodehammer::viewer::ProjectionMode::Orthographic;
        } else if (value == "perspective") {
            cam.projection = nodehammer::viewer::ProjectionMode::Perspective;
        }
    }
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
    if (std::string cull_str; readField(j, "cull", cull_str)) {
        using nodehammer::viewer::CullOverride;
        if (cull_str == "force-on") {
            cfg.cull = CullOverride::ForceCull;
        } else if (cull_str == "force-off") {
            cfg.cull = CullOverride::ForceNoCull;
        } else {
            cfg.cull = CullOverride::Auto;
        }
        cfg.startup_overrides.cull = cfg.cull;
    }
    if (readField(j, "pauseWhenUnfocused", cfg.pause_when_unfocused)) {
        cfg.startup_overrides.pause_when_unfocused = cfg.pause_when_unfocused;
    }
    if (readField(j, "autoOrbit", cfg.auto_orbit)) {
        cfg.startup_overrides.auto_orbit = cfg.auto_orbit;
    }
    if (readField(j, "orbitSpeed", cfg.auto_orbit_speed_deg)) {
        cfg.startup_overrides.auto_orbit_speed_deg = cfg.auto_orbit_speed_deg;
    }
    if (readField(j, "angleCut", cfg.angle_cut)) {
        cfg.startup_overrides.angle_cut = cfg.angle_cut;
    }
    if (readField(j, "shaderAngleCut", cfg.shader_angle_cut)) {
        cfg.startup_overrides.shader_angle_cut = cfg.shader_angle_cut;
    }
    if (readField(j, "booleanCut", cfg.boolean_cut)) {
        cfg.startup_overrides.boolean_cut = cfg.boolean_cut;
    }
    if (readField(j, "cutStart", cfg.angle_cut_start_deg)) {
        cfg.startup_overrides.angle_cut_start_deg = cfg.angle_cut_start_deg;
    }
    if (readField(j, "cutEnd", cfg.angle_cut_end_deg)) {
        cfg.startup_overrides.angle_cut_end_deg = cfg.angle_cut_end_deg;
    }
    if (readField(j, "pbr", cfg.enable_pbr)) {
        cfg.startup_overrides.enable_pbr = cfg.enable_pbr;
    }
    cfg.initial_camera = parseCamera(j);
    cfg.startup_overrides.camera = cfg.initial_camera;

    std::string archiveUrl;
    bool lock = true; // viewer-mode publications are content-locked by default
    readField(j, "archive", archiveUrl);
    readField(j, "lock", lock);

    nodehammer::viewer::App::Handle application(cfg);

    if (!archiveUrl.empty()) {
        // Viewer mode: fetch the .nhproj bytes; the callback installs a Remote,
        // content-locked project. The App runs with its empty working set until the
        // archive lands (a brief loading state). `lock` rides along in userData.
        emscripten_fetch_attr_t attr;
        emscripten_fetch_attr_init(&attr);
        std::strcpy(attr.requestMethod, "GET");
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
        attr.userData = reinterpret_cast<void *>(static_cast<std::uintptr_t>(lock ? 1 : 0));
        attr.onsuccess = [](emscripten_fetch_t *f) {
            auto *app = nodehammer::viewer::App::instance();
            if (app != nullptr) {
                const bool locked = reinterpret_cast<std::uintptr_t>(f->userData) != 0;
                app->openArchiveRemote(
                    std::as_bytes(std::span{reinterpret_cast<const std::byte *>(f->data),
                                            static_cast<std::size_t>(f->numBytes)}),
                    locked);
            }
            emscripten_fetch_close(f);
        };
        attr.onerror = [](emscripten_fetch_t *f) {
            std::fprintf(stderr, "viewer: failed to fetch archive (HTTP %d)\n", f->status);
            emscripten_fetch_close(f);
        };
        emscripten_fetch(&attr, archiveUrl.c_str());
    } else {
        // Application mode (no sidecar-provided archive): the App keeps its
        // eagerly-allocated empty working set. Kick an async IndexedDB restore so a
        // previously-persisted project reappears; drops/picks otherwise accumulate
        // into the working set and App-side recognition picks the root keys.
        application->restoreWebProjectFromIdb();
    }

    application->run();
    return 0;
}

} // extern "C"

// Empty main() so emscripten's INVOKE_RUN path completes cleanly without
// doing anything. The viewer is started later via nh_viewer_start; this
// keeps the runtime alive (paired with EXIT_RUNTIME=0).
int main(int /*argc*/, char ** /*argv*/) { return 0; }
