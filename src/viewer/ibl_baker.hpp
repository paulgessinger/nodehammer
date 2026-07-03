#pragma once

#include "ibl.hpp"

#include <chrono>
#include <functional>

namespace nodehammer::viewer {

/// Owns the viewer's IBL debounce → dirty-track → bake → install loop, pulled
/// out of `App::Impl::onFrame()`. The GPU bake and the notification surface stay
/// in the App (injected as the `BakeFn`); this controller owns only the timing
/// and the "installed / needs-rebake" state — the former `ibl_*` member cluster.
///
/// Mirrors the sibling-controller pattern (Camera / BuildSession): a member the
/// App constructs, wires with a callback, and drives once per frame.
class IblBaker {
  public:
    /// Bakes the IBL for `settings` on the GPU and installs it into the
    /// renderers, plus the first/rebake logging + notification. `first` marks
    /// the initial bake (no "rebake complete" toast). Supplied by the App, which
    /// owns the GPU and the notification surface.
    using BakeFn = std::function<void(const IblSettings &settings, bool first)>;

    void setBake(BakeFn bake) { bake_ = std::move(bake); }

    /// Debounced auto-rebake, called every frame. Resets the settle timer
    /// whenever `settings` changes; once they've been stable for the debounce
    /// window and differ from the last bake — or a rebake was forced, or nothing
    /// is installed yet — runs the bake.
    void poll(const IblSettings &settings, std::chrono::steady_clock::time_point now);

    /// Queue a rebake for the next `poll()` (the UI "Rebake IBL" action).
    void requestRebake() { rebake_pending_ = true; }

    /// Still-needs-work predicate (replaces the old `iblDirty()`): a forced
    /// rebake is queued, or the live settings differ from the last bake.
    [[nodiscard]] bool dirty(const IblSettings &settings) const {
        return rebake_pending_ || settings != last_baked_;
    }

    /// Whether a bake has ever completed (the first frame draws against 1×1
    /// placeholder IBL until this flips true).
    [[nodiscard]] bool installed() const { return installed_; }

  private:
    // Debounce window: how long IBL settings must be stable before an automatic
    // rebake fires (so dragging a sun-direction slider bakes once on release,
    // not every frame).
    static constexpr std::chrono::milliseconds kRebakeDebounce{300};

    BakeFn bake_;
    bool installed_{false};
    bool rebake_pending_{false};
    IblSettings last_seen_{};
    IblSettings last_baked_{};
    std::chrono::steady_clock::time_point settle_at_{};
};

} // namespace nodehammer::viewer
