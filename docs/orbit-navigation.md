# Orbit Navigation

The viewer navigates with an **orbit / arc-ball camera**: the camera circles a
fixed target point rather than walking through the scene first-person. This
suits the viewer's job — inspecting a static piece of geometry from the outside.

## How to orbit

The camera always points at a target in the middle of your geometry. You move
*around* that target — you never fly through the scene.

| To…              | Mouse                       | Trackpad                          |
|------------------|-----------------------------|-----------------------------------|
| **Rotate** (orbit) | Drag with the **left** button | Two-finger **scroll**             |
| **Zoom**         | **Scroll wheel**            | **Pinch**, or scroll while holding **Ctrl** / **⌘** |
| **Pan** (slide)  | Drag with the **middle** button | Two-finger drag while holding **Shift** |

Tips:

- **Left-drag to look around.** Drag left/right to swing around the object, up/down
  to tilt over and under it. You can't quite reach straight overhead or straight
  below — the tilt stops just short of the poles so the view never flips.
- **Scroll to zoom.** Each wheel notch moves about 10% closer or further. Zoom
  speed scales with how far out you are, so it stays smooth from far away down to
  a tight close-up. You can't zoom straight through the center of the object.
- **Middle-drag — or Shift + two-finger swipe — to pan.** This slides the whole
  view sideways/up/down without rotating — handy for re-centering on a detail
  before zooming in. The Shift + swipe combo mirrors Blender. Pan speed scales
  with distance, so it feels the same whether you're near or far.
- **Lost?** Re-framing the geometry recenters the target and fits everything back
  in view.

The model lives in two files:

- [`src/viewer/camera.hpp`](../src/viewer/camera.hpp) — the `Camera` struct and its mutators.
- [`src/viewer/camera.cpp`](../src/viewer/camera.cpp) — the math.

Input is mapped onto the camera in
[`src/viewer/app.cpp`](../src/viewer/app.cpp) (`App::Impl::updateCameraInput`).

## State

The camera *is* a spherical coordinate around a target. That's the whole model —
everything else (eye position, view/projection matrices) is derived on demand.

| Field        | Type            | Default            | Meaning |
|--------------|-----------------|--------------------|---------|
| `target`     | `glm::vec3`     | `{0, 0, 0}`        | The orbit center; the point the camera looks at. |
| `distance`   | `float`         | `10`               | Radial distance from `target` to the eye. |
| `yaw`        | `float` (rad)   | `0`                | Rotation around world **Y**. Normalized to `(-π, π]`. |
| `pitch`      | `float` (rad)   | `0`                | Elevation around the camera-right axis. Clamped to ~`±89°`. |
| `projection` | `ProjectionMode`| `Perspective`      | `Perspective` or `Orthographic`. |
| `fov_deg`    | `float`         | `55`               | Vertical field of view, in `(1, 179)`. |
| `near_plane` | `float`         | `0.05`             | Near clip plane. |
| `far_plane`  | `float`         | `1000`             | Far clip plane (`> near_plane`). |

Note what is **not** state: the scene's framing radius. It is derived from the
loaded geometry and passed into `dolly()` / returned from `frameBounds()`. Keeping
it out of `Camera` means the struct is pure user-facing state that round-trips
cleanly through persistence — no stale geometry-derived values get saved.

### Eye position

The eye is reconstructed from the spherical state every frame
([`camera.cpp:74`](../src/viewer/camera.cpp#L74)):

```cpp
dir = { cos(pitch) * sin(yaw),   // X
        sin(pitch),              // Y (elevation)
        cos(pitch) * cos(yaw) }; // Z
eye = target + dir * distance;
```

The view matrix is `glm::lookAt(eye(), target, +Y)` — i.e. world up is fixed to
`+Y`. Because pitch is clamped below the pole, `eye` and the up vector are never
collinear, so the look-at never degenerates (gimbal lock).

## Invariants

`Camera::sanitize()` ([`camera.cpp:36`](../src/viewer/camera.cpp#L36)) clamps every
field into its valid range and resets any `NaN`/`Inf` field to its default. It is
idempotent and returns `true` if anything was out of spec (so callers can warn).
Call it after loading persisted state or after any external bulk edit (e.g. ImGui
sliders). The mutators below already preserve these invariants on their own.

- `target` — every component finite.
- `distance` — finite, `> 1e-4`.
- `yaw` — finite, wrapped to `(-π, π]`.
- `pitch` — finite, in `(-1.553343, +1.553343)` rad (≈ `±89°`).
- `fov_deg` — finite, in `(1, 179)`.
- `near_plane` — finite; `> 0` for perspective (the projection diverges at the
  eye). Orthographic **allows a negative near** so that, when the user dollies in
  past the scene center, geometry on the camera side of the target is not clipped.
- `far_plane` — finite, `> near_plane`.

## Operations

### Orbit — `orbit(dx_radians, dy_radians)`

```cpp
yaw   = wrapPi(yaw - dx_radians);
pitch = clamp(pitch + dy_radians, -k_pitch_limit, k_pitch_limit);
```

Horizontal input rotates yaw (negated, so a leftward drag swings the scene the
expected way); vertical input tilts pitch, clamped just short of the poles. The
inputs are already in radians — the caller bakes its pixel→radian sensitivity in.

### Dolly (zoom) — `dolly(factor, scene_radius = 0)`

Multiplies `distance` by `factor`: `factor > 1` zooms out, `< 1` zooms in,
`= 1` leaves distance unchanged (used to re-derive clip planes against a new
scene). The interesting part is the **adaptive near/far planes**:

- When `scene_radius > 0`, distance is floored at `scene_radius * 1e-3` so the
  user can't dolly straight through the scene center. The near/far planes then
  **hug the geometry**: with a pad of `3 × scene_radius`, near sits just in front
  of the closest point and far just past the furthest. Paired with reversed-Z
  this yields ~6 orders of magnitude of usable depth precision instead of the ~3
  you get from a wide distance-relative pair. (The 3× pad keeps geometry off the
  exact depth-clear value, which the composite background dome uses to detect
  background.)
  - **Perspective**: `near = max(distance - pad, …, k_min_near)`, `far = distance + pad`.
  - **Orthographic**: `near = distance - pad` (may go negative), `far = distance + pad`.
- When `scene_radius` is unknown (`0`), it falls back to distance-relative sizing:
  `near = distance * 1e-3`, `far ≥ distance * 100`.

### Pan — `pan(dx_world, dy_world)`

Slides the **target** (and so the eye with it) in the camera's right/up plane,
leaving distance and angles untouched:

```cpp
fwd   = normalize(target - eye());
right = normalize(cross(fwd, +Y));
up    = cross(right, fwd);
target += right * dx_world + up * dy_world;
```

### Frame bounds — `frameBounds(min, max, margin = 1.2)`

Auto-frames an axis-aligned bounding box: sets `target` to the box center and
`distance` so the box fits in view at the current FOV, scaled by `margin`. It
also seeds near/far (pad `1.5 × scene_radius`) and returns the scene framing
radius (the bounding-sphere half-diagonal) so the caller can stash it for later
`dolly()` calls.

## Input mapping

`App::Impl::updateCameraInput()`
([`app.cpp:619`](../src/viewer/app.cpp#L619)) translates pointer, wheel, trackpad,
and gesture input into the operations above. All of it is skipped when ImGui wants
the mouse (`io.WantCaptureMouse`), so dragging on a panel doesn't move the camera.

| Input                         | Action | Sensitivity |
|-------------------------------|--------|-------------|
| **Left-drag**                 | Orbit  | `0.005 rad/px` (tuned for ~1k–1.5k px windows). |
| **Middle-drag**               | Pan    | `distance × 0.001` per px — scales with zoom so motion feels constant at any range. |
| **Mouse wheel**               | Dolly  | `distance *= 1.1^(-notch)` — each notch ≈ 10% closer/further (Blender-like). |
| **Wheel + Ctrl/⌘**            | Dolly  | Same as wheel; the modifier forces zoom even in trackpad mode. |
| **Trackpad two-finger scroll**| Orbit  | `0.08 rad` on web, `0.03 rad` native. |
| **Trackpad scroll + Shift**   | Pan    | `distance × 0.0027` per unit web, `× 0.01` native — mirrors Blender. |
| **Pinch gesture**             | Dolly  | `distance *= 1 / scale_delta` (scale_delta clamped to `[0.05, 20]`). |

### Wheel vs. trackpad

A discrete mouse wheel zooms; a smooth trackpad scroll orbits. The viewer can't
tell them apart from the OS event type alone, so `classifyScroll()`
([`app.cpp:537`](../src/viewer/app.cpp#L537)) runs a small heuristic with hysteresis:
integer-step vertical-only deltas score as **wheel**, while fractional or
horizontal deltas score as **trackpad**. The zoom modifier (`Ctrl`/`Super`,
`isZoomModifier`) always overrides this to force a dolly; the pan modifier
(`Shift`, `isPanModifier`) reroutes a trackpad scroll from orbit to pan.

### Kinetic-scroll tail

macOS trackpads keep emitting decaying "momentum" scroll events for a moment
after the fingers lift. That tail belongs to the gesture that spawned it, but it
carries whatever modifiers are held *now* — so a modifier change during the tail
would flip the mode mid-glide and lurch the camera. Both directions bite:
releasing **Shift** at the end of a pan drops the tail into **orbit**, and
pressing **Shift** into an orbit's tail jumps it into a **pan**.

`handleScrollEvent()`
([`app.cpp:544`](../src/viewer/app.cpp#L544)) defends against this by treating a
**scroll sequence** as one gesture with one mode. A sequence is a run of events
less than `0.1 s` apart (one physical gesture plus its momentum tail); its mode
(pan / zoom / orbit, set by the held modifiers) is **locked at the first event**.
If the modifiers later diverge from that start state — in either direction — the
rest of the tail is **swallowed** rather than re-interpreted. Holding the
modifier steady through the tail still glides to a stop; changing it cancels the
glide. (The OS doesn't surface a momentum-phase flag through sokol's scroll
event, so the timing gap is the only signal available.)

## Auto-orbit

A presentation mode that spins the camera on its own. Config in
[`config.hpp`](../src/viewer/config.hpp):

| Setting                | Default | Meaning |
|------------------------|---------|---------|
| `auto_orbit`           | `false` | Enable continuous rotation. |
| `auto_orbit_speed_deg` | `15`    | Degrees per second of yaw. |

Each frame, when enabled, the app calls
`camera.orbit(radians(auto_orbit_speed_deg) * delta_seconds, 0)` — yaw only,
pitch untouched.

## Persisting and restoring camera state

Because `Camera` is pure user-facing state, it serializes cleanly. A start-up
camera can be supplied via `cfg.initial_camera`; `applyInitialCamera()`
([`app.cpp:670`](../src/viewer/app.cpp#L670)) copies it in, calls `sanitize()`, then
`dolly(1.f, scene_radius)` to **re-derive** the near/far/distance clamps against
the *current* scene rather than whatever was true when the state was saved.

## Constants

| Constant                     | Value             | Where | Purpose |
|------------------------------|-------------------|-------|---------|
| `k_pitch_limit`              | `1.553343` rad    | [camera.cpp:12](../src/viewer/camera.cpp#L12) | Pitch clamp (~89°), avoids gimbal lock. |
| `k_fov_min_deg` / `k_fov_max_deg` | `1` / `179`  | [camera.cpp:13](../src/viewer/camera.cpp#L13) | FOV bounds. |
| `k_min_near`                 | `1e-3`            | [camera.cpp:15](../src/viewer/camera.cpp#L15) | Minimum perspective near plane. |
| `k_min_distance`             | `1e-4`            | [camera.cpp:16](../src/viewer/camera.cpp#L16) | Minimum orbit distance. |
| Mouse orbit sensitivity      | `0.005 rad/px`    | [app.cpp:631](../src/viewer/app.cpp#L631) | Left-drag. |
| Mouse pan sensitivity        | `distance × 0.001`| [app.cpp:636](../src/viewer/app.cpp#L636) | Middle-drag. |
| Wheel zoom base              | `1.1`             | [app.cpp:645](../src/viewer/app.cpp#L645) | ~10% per notch. |
| Trackpad orbit sensitivity   | `0.08` web / `0.03` native | [app.cpp:654](../src/viewer/app.cpp#L654) | Two-finger scroll. |
| Trackpad pan sensitivity     | `distance × 0.0027` web / `× 0.01` native | [app.cpp:651](../src/viewer/app.cpp#L651) | Shift + two-finger scroll. |
| Scroll sequence gap          | `0.1 s`           | [app.cpp:544](../src/viewer/app.cpp#L544) | Momentum-tail cancel window. |
| Dolly distance floor         | `scene_radius × 1e-3` | [camera.cpp:133](../src/viewer/camera.cpp#L133) | Don't dolly through center. |
| Dolly near/far pad           | `scene_radius × 3.0` | [camera.cpp:145](../src/viewer/camera.cpp#L145) | Clip-plane envelope. |
| Frame-bounds pad             | `scene_radius × 1.5` | [camera.cpp:185](../src/viewer/camera.cpp#L185) | Initial clip-plane envelope. |
