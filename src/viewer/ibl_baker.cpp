#include "ibl_baker.hpp"

namespace nodehammer::viewer {

void IblBaker::poll(const IblSettings &settings, std::chrono::steady_clock::time_point now) {
    // Auto-rebake on settings change, debounced. Each frame, if the user has
    // touched any IBL slider since last frame, reset the settle timer. Once
    // settings have been stable for the debounce window and differ from the last
    // bake, queue a rebake. Manual "Rebake IBL" flows through the same flag.
    if (settings != last_seen_) {
        last_seen_ = settings;
        settle_at_ = now + kRebakeDebounce;
    }
    if (installed_ && !rebake_pending_ && settings != last_baked_ && now >= settle_at_) {
        rebake_pending_ = true;
    }

    // Procedural IBL bake — the App's BakeFn runs it on the GPU (first frame,
    // then on each debounced rebake) and installs it into both renderers.
    if (!installed_ || rebake_pending_) {
        const bool first = !installed_;
        if (bake_) {
            bake_(settings, first);
        }
        installed_ = true;
        rebake_pending_ = false;
        last_baked_ = settings;
        last_seen_ = settings;
    }
}

} // namespace nodehammer::viewer
