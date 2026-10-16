# Embedding the web viewer (embed mode)

Embed mode lets a host page put the wasm viewer in an `<iframe>` and treat it
as a pure 3D surface: the host owns the page chrome, the URL, any persistence,
and the loading experience. The intended use is a cross-fade from a lightweight
preview (e.g. a three.js render of the same geometry) into the full viewer, so
the two sides have to agree exactly on what the camera numbers mean. That
agreement is the bulk of this document.

## Turning it on

```
index.html?archive=detector.nhproj&embed=1&cameraTargetX=0&cameraTargetY=0&cameraTargetZ=0&cameraDistance=1200&cameraYaw=35&cameraPitch=20
```

`?embed=1` (or `"defaults": { "embed": true }` in `nh_manifest.json`) sets
`viewerOpts.embed`, which `src/web/viewer_main.cpp` reads into
`viewer::Config::presentation = PresentationMode::Surface` and attaches a web
connection. In embed mode the viewer:

- draws **only the 3D scene** — no Dear ImGui dockspace, panels, menu bar,
  toasts, modals or drag-drop overlay, and the shell's `#status` text is hidden.
  Orbit (left-drag), pan (middle-drag / shift-scroll), zoom (wheel, pinch) all
  still work;
- **never rewrites `window.location`** (no `history.replaceState` URL sync);
- **never reads or writes `localStorage` or IndexedDB** — no persisted viewer
  state, render-quality profile, ImGui layout or working-set restore. Every
  launch starts from the archive manifest's `[view]` steer plus the query
  parameters;
- talks to the parent over `window.postMessage` (below). The bridge is inert
  when the page is not actually framed (`window.parent === window`).

Everything else (`?pbr=`, `?autoOrbit=`, the cut parameters, `?lock=`,
`?backend=`) behaves as in the normal shell. There is **no** `?bg=` background
colour parameter: the background is the IBL sky dome composited through the
viewer's exposure/tonemap chain, not a flat clear colour, so a host-matching
colour could not be honoured exactly without shader changes. Fade from a
preview whose background approximates the viewer's default look, or fade over
an opaque host background.

## Message protocol

All messages are plain objects with a string `type`. The viewer posts to
`window.parent` with target origin `*`; a host should filter on
`event.source === iframe.contentWindow`. Incoming messages are accepted from
any origin (the only effect is moving the camera of a page the host already
controls).

### Viewer → host

| type | when | payload |
|---|---|---|
| `nh:ScenePresented` | **exactly once**, after the scene build finished, its meshes were uploaded to the GPU, **and the first frame showing them has been rendered**. Not at runtime init and not when the build merely completes. | `{ type: 'nh:ScenePresented', camera }` — the pose that first frame was drawn with (after auto-framing and the URL camera, if any). |
| `nh:CameraChanged` | whenever the pose differs from the last one posted, throttled to **10 Hz** (one post per 100 ms at most; the final pose after a gesture is always delivered). Never before `nh:ScenePresented`. | `{ type: 'nh:CameraChanged', camera }` |
| `nh:CameraResponse` | Immediately in reply to `nh:GetCamera`, after initial readiness. | `{ type: 'nh:CameraResponse', requestId, camera }` |

Wire types use `nh:` plus the C++ struct name. Commands live in `viewer::commands`,
events in `viewer::events`. `requestId` defaults to zero and must be a nonnegative
JavaScript-safe integer (at most 2^53 − 1). The earlier draft names (`nh:ready`,
`nh:camera`, `nh:setCamera`, `nh:getCamera`) are replaced, not accepted as aliases.

### Host → viewer

| type | effect |
|---|---|
| `{ type: 'nh:SetCamera', camera, animate? }` | Set the pose. `camera` must carry all six numeric fields (`targetX, targetY, targetZ, distance > 0, yawDeg, pitchDeg`); `projection` is optional. With `animate: true` the viewer eases from the current pose over ~0.4 s (smoothstep; shortest yaw arc; distance in log space) and user input cancels the ease. A `SetCamera` that arrives before `nh:ScenePresented` becomes the initial camera applied after auto-framing. Messages failing validation are ignored. |
| `{ type: 'nh:GetCamera', requestId? }` | Reply with one `nh:CameraResponse` carrying the same `requestId` (no-op before `nh:ScenePresented`, whose payload already carries the pose). |

### The `camera` object

```json
{
  "targetX": 0.0, "targetY": 0.0, "targetZ": 0.0,
  "distance": 1200.0,
  "yawDeg": 35.0, "pitchDeg": 20.0,
  "projection": "perspective"
}
```

This is the same vocabulary as the query parameters minus their `camera`
prefix (`cameraTargetX` ↔ `targetX`, `cameraYaw` ↔ `yawDeg`, …) and as the
`camera` option of `nh_viewer_start`; it is produced and parsed by
`src/viewer/camera_json.hpp`. `projection` is `"perspective"` (default) or
`"orthographic"` (`"ortho"` accepted on input). Field-of-view and clip planes
are **not** part of the exchange — the viewer re-derives them from the loaded
scene.

## Camera convention (precise)

The viewer's `Camera` (`src/viewer/camera.hpp`) is an orbit camera described
by a target point and spherical coordinates of the eye around it.

- **Coordinate frame**: right-handed, **+Y is up**, identical to three.js's
  default world frame. Positions are the render-IR world coordinates of the
  loaded geometry — the viewer applies no unit scaling, so `target*` and
  `distance` are in whatever units the source geometry uses (ROOT/TGeo and
  DD4hep: centimetres; the glTF exporter's `unit_scale` is export-only and
  does not affect the viewer).
- **Eye position** (`Camera::eye()`):

  ```
  dir = ( cos(pitch)·sin(yaw),  sin(pitch),  cos(pitch)·cos(yaw) )
  eye = target + distance · dir
  ```

  with yaw/pitch in radians (`yawDeg`/`pitchDeg` × π/180). The view matrix is
  `lookAt(eye, target, up = (0, 1, 0))`.
- **`distance`**: eye-to-target length, > 0.
- **`yawDeg`**: rotation of the eye about the world **+Y** axis. `0` puts the
  eye on the **+Z** side of the target looking toward −Z; `+90` puts it on the
  **+X** side. Positive yaw is a right-handed (counter-clockwise when viewed
  from +Y) rotation that carries +Z toward +X. Reported values are wrapped to
  (−180, 180]. Dragging the mouse to the right *decreases* yaw.
- **`pitchDeg`**: elevation of the eye above the target's horizontal (XZ)
  plane. `0` is level; positive raises the eye above the target so it looks
  down; clamped to ±89°.
- **Perspective**: vertical field of view **55°** (fixed), aspect ratio of the
  canvas, so for a canvas of size `w × h` the horizontal FOV is
  `2·atan(tan(27.5°)·w/h)`.
- **Orthographic**: the visible half-height is `distance · tan(27.5°)` (so the
  same `distance` frames the same vertical extent as the perspective camera
  at the target plane), half-width = half-height × aspect. The eye still sits
  at `eye` above; only the projection changes.

### Mapping to three.js

three.js shares the frame, so no axis conversion is needed:

```js
const yaw = THREE.MathUtils.degToRad(cam.yawDeg);
const pitch = THREE.MathUtils.degToRad(cam.pitchDeg);
const target = new THREE.Vector3(cam.targetX, cam.targetY, cam.targetZ);
const dir = new THREE.Vector3(Math.cos(pitch) * Math.sin(yaw), Math.sin(pitch),
                              Math.cos(pitch) * Math.cos(yaw));
camera.position.copy(target).addScaledVector(dir, cam.distance);
camera.up.set(0, 1, 0);
camera.lookAt(target);
// PerspectiveCamera(55, canvas.width / canvas.height, near, far)
```

In terms of `THREE.Spherical` (`theta` measured from +Z toward +X around Y,
`phi` the polar angle from +Y): `theta = yaw` and `phi = 90° − pitch`, with
`radius = distance`. The inverse, for a three.js `OrbitControls` host feeding
`nh:SetCamera`:

```js
const sph = new THREE.Spherical().setFromVector3(
    camera.position.clone().sub(controls.target));
const pose = {
  targetX: controls.target.x, targetY: controls.target.y, targetZ: controls.target.z,
  distance: sph.radius,
  yawDeg: THREE.MathUtils.radToDeg(sph.theta),
  pitchDeg: 90 - THREE.MathUtils.radToDeg(sph.phi),
  projection: 'perspective',
};
```

## Host page example

```html
<div id="stage" style="position:relative; width:960px; height:540px;">
  <canvas id="preview" style="position:absolute; inset:0;"></canvas>   <!-- three.js -->
  <iframe id="viewer" style="position:absolute; inset:0; border:0; width:100%; height:100%;
                             opacity:0; transition:opacity 600ms ease;"
          allow="cross-origin-isolated"
          src="https://example.org/viewer/index.html?archive=detector.nhproj&embed=1
               &cameraTargetX=0&cameraTargetY=0&cameraTargetZ=0
               &cameraDistance=1200&cameraYaw=35&cameraPitch=20"></iframe>
</div>
<script>
  const iframe = document.getElementById('viewer');
  window.addEventListener('message', (ev) => {
    if (ev.source !== iframe.contentWindow || !ev.data || typeof ev.data.type !== 'string') {
      return;
    }
    if (ev.data.type === 'nh:ScenePresented') {
      applyPoseToPreview(ev.data.camera);   // make both sides agree, then fade
      iframe.style.opacity = '1';
    } else if (ev.data.type === 'nh:CameraChanged' || ev.data.type === 'nh:CameraResponse') {
      applyPoseToPreview(ev.data.camera);   // keep the preview in step (optional)
    }
  });

  // Later: drive the viewer from the host.
  iframe.contentWindow.postMessage({
    type: 'nh:SetCamera', animate: true,
    camera: { targetX: 0, targetY: 0, targetZ: 0, distance: 800,
              yawDeg: -45, pitchDeg: 10, projection: 'perspective' },
  }, '*');
  iframe.contentWindow.postMessage({ type: 'nh:GetCamera' }, '*');
</script>
```

Hand the iframe the same camera on its URL that the preview is showing, so
the `nh:ScenePresented` fade reveals only added detail. Because `nh:ScenePresented` fires after
the first frame containing the scene has been *rendered*, there is no flash of
empty canvas when the opacity transition starts.

## Shared viewer control

External control is independent of both platform and presentation. `Config::connection`
accepts a shared `ViewerConnection`, a typed, thread-safe mailbox:

- Commands: `SetCamera { pose, animate }`, `GetCamera { request_id }`.
- Events: `ScenePresented { camera }`, `CameraChanged { camera }`,
  `CameraResponse { request_id, camera }`.

The App drains commands before its idle gates and executes them on the viewer
thread. A queued command wakes an unfocused viewer on the next frame callback.
A `SetCamera` received before framing becomes the initial pose and takes precedence
over archive defaults. `GetCamera` returns the current camera, including before
scene readiness; the web adapter suppresses responses until `nh:ScenePresented` to preserve
its wire protocol. Animated commands start a 0.4-second transition measured from
command execution, and mouse/scroll/gesture input cancels it.

`ScenePresented` follows the first rendered frame after scene framing (including
reframing after a rebuild). `CameraChanged` reports subsequent presented poses.
The mailbox coalesces consecutive camera changes to the latest pose, preserving
responses and scene events in order. It has one event consumer; adapters own
client fan-out and map request IDs to their clients. Commands and responses are
not dropped, so adapters must drain events and apply ingress limits appropriate
to their transport.

A native host can supply the same mailbox without changing the viewer's UI:

```cpp
auto connection = std::make_shared<viewer::ViewerConnection>();
viewer::Config cfg;
cfg.connection = connection; // Interactive presentation remains the default.
viewer::App::Handle application(cfg);
// Run the viewer on its owning thread.
application->run();
```

A controller/server thread that holds `connection` can call
`connection->submit(viewer::commands::SetCamera{pose, true})`, submit `viewer::commands::GetCamera{request_id}`,
and drain `takeEvents()` without accessing `App` or GPU state. The host owns the
server's lifetime and shuts it down when the viewer exits. No server or networking
library is included here.

`ViewerConnection::poll()` is an optional nonblocking adapter hook called on the
viewer thread, even while rendering is idle. The web subclass uses it to drain
events, translate the first `ScenePresented` into `nh:ScenePresented`, and deliver the latest
camera at 10 Hz (including the final pose after a gesture). A server with its own
thread can use the base mailbox directly; it does not need this hook.

`PresentationMode::Surface` suppresses UI and persistence on either platform.
Attaching external control alone does not select it. The `?embed=1` web startup
option chooses both surface presentation and the postMessage adapter as a convenience.
The web adapter owns listener installation/removal, JSON parsing, and wire protocol
state; CMake excludes its source from native builds.

## Implementation map

| concern | where |
|---|---|
| web option plumbing | `web/index.html` → `src/web/viewer_main.cpp` selects surface presentation and creates the web connection |
| UI / URL / storage policy | `PresentationMode` in `src/viewer/config.hpp`, compared directly in `App` (Config is data-only) |
| typed commands / events, thread-safe mailbox | `src/viewer/viewer_messages.hpp` (commands/events), `viewer_connection.hpp` and `.cpp` (mailbox) |
| command execution / presented-frame events | `App::Impl::processCommands`, `publishPresentedFrame` in `src/viewer/app.cpp` |
| reusable camera motion | `App::setCameraPose`, `CameraTransition` in `src/viewer/camera_transition.hpp` and `.cpp` |
| web postMessage adapter and exports | `src/viewer/web_embed_connection.cpp`, compiled only for Emscripten |
| camera JSON vocabulary | `src/viewer/camera_json.hpp`; shared by the web startup parser and adapter |

JSON command parsing and event serialization live in `viewer_protocol.hpp` / `.cpp`.
The adapter forwards whole envelopes through one C export and dispatches delivery
policy with `std::visit`; it does not maintain a second mapping of message names.
