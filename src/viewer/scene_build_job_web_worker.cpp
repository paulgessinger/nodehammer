#include "scene_build_job.hpp"

#include "scene_build_job_internal.hpp"
#include "scene_build_job_web_backend.hpp"

#include <config/config_writer.hpp>
#include <ir/diagnostic_codes.hpp>
#include <ir/fb/render/flatbuffer.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <scene_build.hpp>

#include <emscripten/emscripten.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

// ── JS bridge ─────────────────────────────────────────────────────────────────
//
// The viewer (this module) owns a Web Worker that hosts a *second* wasm
// instance (compute_worker.js + nodehammer-compute). Worker messages arrive on
// the main thread's event loop between frames; the onmessage handler stashes
// status into Module.__nhWorker, which the C++ poll() reads back through the
// getters below. Result bytes are copied into *this* module's heap by the
// handler so C++ can deserialize them directly.

// clang-format off
// 1 if Web Workers are usable and not force-disabled (?compute=main/cooperative).
EM_JS(int, nh_worker_probe, (), {
    try {
        var forced = "";
        try {
            forced = (new URLSearchParams(self.location.search).get("compute") || "").toLowerCase();
        } catch (e) { /* no location (non-browser) — fall through */ }
        if (forced === "main" || forced === "cooperative") return 0;
        return (typeof Worker === "function") ? 1 : 0;
    } catch (e) { return 0; }
});

// Create the worker and wire its onmessage. Returns 1 on success.
EM_JS(int, nh_worker_create, (), {
    try {
        if (Module.__nhWorker && Module.__nhWorker.worker) return 1;
        var st = {
            worker: null,
            status: 0,      // 0 idle, 1 building, 2 done, 3 error
            resultPtr: 0,
            resultLen: 0,
            error: "",
            fatal: 0,       // 1 = worker/module is broken; main thread should fall back
            phase: 0,       // 1 prep, 2 cut, 3 tess, 4 finalize
            processed: 0,
            total: 0,
        };
        var w = new Worker("compute_worker.js");
        w.onmessage = function (e) {
            var m = e.data || {};
            // Quoted keys: messages cross the worker boundary between separate
            // Closure compilations (viewer / compute module) and the
            // non-Closure'd compute_worker.js. Unquoted keys get renamed
            // inconsistently and the protocol breaks in the Release build.
            if (m['nh'] === "progress") {
                st.phase = m['phase']; st.processed = m['processed']; st.total = m['total'];
            } else if (m['nh'] === "error") {
                st.error = m['message'] || "compute worker error";
                if (m['fatal']) st.fatal = 1;
                st.status = 3;
            } else if (m['nh'] === "result") {
                var bytes = new Uint8Array(m['buffer']);
                var ptr = _malloc(bytes.length);
                HEAPU8.set(bytes, ptr);
                st.resultPtr = ptr;
                st.resultLen = bytes.length;
                st.status = 2;
            }
        };
        w.onerror = function (e) {
            // An error on the worker itself (bad script, importScripts failure)
            // means the worker can't be used — fatal, fall back.
            st.error = "compute worker error: " + (e && e.message ? e.message : e);
            st.fatal = 1;
            st.status = 3;
        };
        st.worker = w;
        Module.__nhWorker = st;
        return 1;
    } catch (e) { return 0; }
});

// Post a build request. scene_ptr == 0 means "reuse the cached scene for this
// epoch" (e.g. a wedge re-aim); otherwise the bytes are transferred to the worker.
EM_JS(void, nh_worker_post, (unsigned epoch, const uint8_t *scene_ptr, unsigned scene_len,
                             const char *config_toml, int has_wedge, double start_deg,
                             double end_deg, double margin), {
    var st = Module.__nhWorker;
    st.status = 1; st.error = ""; st.resultPtr = 0; st.resultLen = 0;
    st.phase = 1; st.processed = 0; st.total = 0;
    // Quoted keys: this object is read by the non-Closure'd compute_worker.js,
    // so Closure must not rename the wire names in the Release build.
    var msg = {
        'nh': "build",
        'epoch': epoch >>> 0,
        'wedge': has_wedge ? { 'startDeg': start_deg, 'endDeg': end_deg, 'margin': margin } : null,
    };
    var transfer = [];
    if (scene_ptr) {
        // Copy out of the wasm heap into a standalone, transferable buffer.
        var buf = HEAPU8.slice(scene_ptr, scene_ptr + scene_len).buffer;
        msg['sceneBytes'] = buf;
        msg['configToml'] = config_toml ? UTF8ToString(config_toml) : "";
        transfer.push(buf);
    }
    st.worker.postMessage(msg, transfer);
});

EM_JS(int, nh_worker_status, (), { var st = Module.__nhWorker; return st ? st.status : 0; });
EM_JS(int, nh_worker_phase, (), { var st = Module.__nhWorker; return st ? st.phase : 0; });
EM_JS(double, nh_worker_processed, (), { var st = Module.__nhWorker; return st ? st.processed : 0; });
EM_JS(double, nh_worker_total, (), { var st = Module.__nhWorker; return st ? st.total : 0; });
EM_JS(unsigned, nh_worker_result_ptr, (), { var st = Module.__nhWorker; return st ? st.resultPtr : 0; });
EM_JS(unsigned, nh_worker_result_len, (), { var st = Module.__nhWorker; return st ? st.resultLen : 0; });
EM_JS(int, nh_worker_fatal, (), { var st = Module.__nhWorker; return (st && st.fatal) ? 1 : 0; });

// Copy out the pending error message (malloc'd; caller frees). Empty if none.
EM_JS(char *, nh_worker_take_error, (), {
    var st = Module.__nhWorker;
    var s = (st && st.error) ? st.error : "";
    var len = lengthBytesUTF8(s) + 1;
    var p = _malloc(len);
    stringToUTF8(s, p, len);
    return p;
});

// Reset transient state after C++ has consumed a result/error.
EM_JS(void, nh_worker_reset, (), {
    var st = Module.__nhWorker;
    if (st) { st.status = 0; st.resultPtr = 0; st.resultLen = 0; st.error = ""; }
});
// clang-format on

namespace {

class WorkerBackend final : public IWebBackend {
  public:
    void start(std::shared_ptr<const ::nodehammer::config::NHConfig> config,
               std::shared_ptr<const ::nodehammer::ir::SemanticScene> scene,
               std::string config_label, std::string geometry_label,
               std::optional<::nodehammer::tessellation::WedgeCutParams> wedge_cut) override {
        logPreBuild(config_label, geometry_label);
        result_ = {};

        // Re-serialize only when the scene identity changed. The App hands the
        // *same* pristine shared_ptr back for a wedge re-aim, so pointer
        // identity is a perfect cache key — the worker then reuses its cached
        // deserialized scene and we skip both the serialize and the transfer.
        const bool scene_changed = scene.get() != last_scene_;
        if (scene_changed) {
            ++epoch_;
            last_scene_ = scene.get();
            scene_bytes_ = ::nodehammer::ir::semanticSceneToBytes(*scene);
            config_toml_ = ::nodehammer::config::configToToml(*config);
        }

        const int wedge = wedge_cut ? 1 : 0;
        const double s = wedge_cut ? wedge_cut->startDeg : 0.0;
        const double e = wedge_cut ? wedge_cut->endDeg : 0.0;
        const double m = wedge_cut ? wedge_cut->margin : 2.0;

        if (scene_changed) {
            nh_worker_post(epoch_, reinterpret_cast<const std::uint8_t *>(scene_bytes_.data()),
                           static_cast<unsigned>(scene_bytes_.size()), config_toml_.c_str(), wedge,
                           s, e, m);
        } else {
            nh_worker_post(epoch_, nullptr, 0, nullptr, wedge, s, e, m);
        }
        state_ = State::Building;
    }

    bool poll(std::uint64_t /*budget_ns*/) override {
        if (state_ == State::Idle) {
            return false;
        }
        if (state_ == State::Done) {
            return true;
        }
        // A *fatal* failure (bad worker script, module load/instantiate failure)
        // can surface at any time — including a worker that died before our build
        // message, where a dead worker would otherwise leave status stuck at
        // "building". So check it first. The result is left empty and
        // wantsFallback() is raised so SceneBuildJob reruns this build on the
        // cooperative backend.
        if (nh_worker_fatal() != 0) {
            fatal_ = true;
            char *err = nh_worker_take_error();
            std::free(err);
            result_.scene = nullptr;
            nh_worker_reset();
            state_ = State::Done;
            return true;
        }

        // Building: check what the worker has reported.
        const int status = nh_worker_status();
        if (status == 1) {
            return false; // still working
        }
        if (status == 3) {
            // Non-fatal error: a content failure (bad config/geometry) that the
            // cooperative path would hit too, so report it rather than fall back.
            char *err = nh_worker_take_error();
            result_.scene = nullptr;
            result_.diags.error(::nodehammer::codes::kErrComputeWorker,
                                err != nullptr ? err : "compute worker failed");
            std::free(err);
            nh_worker_reset();
            state_ = State::Done;
            return true;
        }
        if (status == 2) {
            const auto ptr = nh_worker_result_ptr();
            const auto len = nh_worker_result_len();
            try {
                const auto *bytes =
                    reinterpret_cast<const std::byte *>(static_cast<uintptr_t>(ptr));
                auto rendered = ::nodehammer::ir::renderSceneFromBytes(std::span{bytes, len});
                result_.scene =
                    std::make_shared<::nodehammer::ir::RenderScene>(std::move(rendered));
            } catch (const std::exception &ex) {
                result_.scene = nullptr;
                result_.diags.error(::nodehammer::codes::kErrComputeWorker,
                                    std::string{"failed to decode render scene: "} + ex.what());
            }
            std::free(reinterpret_cast<void *>(static_cast<uintptr_t>(ptr)));
            nh_worker_reset();
            state_ = State::Done;
            return true;
        }
        return false;
    }

    ::nodehammer::pipeline::SceneBuildResult take() override {
        ::nodehammer::pipeline::SceneBuildResult out = std::move(result_);
        result_ = {};
        state_ = State::Idle;
        return out;
    }

    // Raised once the worker is known to be unusable (fatal load/run failure);
    // SceneBuildJob then reruns the build on the cooperative backend.
    bool wantsFallback() const override { return fatal_; }

    // Progress getters are phase-gated to match the cooperative/native jobs:
    // only the active phase reports counts.
    std::size_t tessellationTotal() const override {
        return activeBuildPhase() == 3 ? static_cast<std::size_t>(nh_worker_total()) : 0;
    }
    std::size_t tessellationProcessed() const override {
        return activeBuildPhase() == 3 ? static_cast<std::size_t>(nh_worker_processed()) : 0;
    }
    std::size_t wedgeCutTotal() const override {
        return activeBuildPhase() == 2 ? static_cast<std::size_t>(nh_worker_total()) : 0;
    }
    std::size_t wedgeCutProcessed() const override {
        return activeBuildPhase() == 2 ? static_cast<std::size_t>(nh_worker_processed()) : 0;
    }

    SceneBuildJob::Phase phase() const override {
        switch (state_) {
        case State::Idle:
            return SceneBuildJob::Phase::Idle;
        case State::Done:
            return SceneBuildJob::Phase::Done;
        case State::Building:
            switch (activeBuildPhase()) {
            case 2:
                return SceneBuildJob::Phase::Cutting;
            case 3:
                return SceneBuildJob::Phase::Tessellating;
            case 4:
                return SceneBuildJob::Phase::Finalizing;
            default:
                return SceneBuildJob::Phase::Preparing;
            }
        }
        return SceneBuildJob::Phase::Idle;
    }

  private:
    enum class State : std::uint8_t { Idle, Building, Done };

    // Phase reported by the worker, or 0 when not actively building.
    int activeBuildPhase() const { return state_ == State::Building ? nh_worker_phase() : 0; }

    State state_{State::Idle};
    ::nodehammer::pipeline::SceneBuildResult result_;
    bool fatal_{false};

    // Pristine-scene cache (main-thread side): identity + serialized bytes so a
    // re-aim reuses them instead of re-serializing.
    const ::nodehammer::ir::SemanticScene *last_scene_{nullptr};
    std::uint32_t epoch_{0};
    std::vector<std::byte> scene_bytes_;
    std::string config_toml_;
};

} // namespace

std::unique_ptr<IWebBackend> makeWorkerBackend() {
    if (nh_worker_probe() == 0) {
        return nullptr;
    }
    if (nh_worker_create() == 0) {
        return nullptr;
    }
    return std::make_unique<WorkerBackend>();
}

} // namespace nodehammer::viewer
