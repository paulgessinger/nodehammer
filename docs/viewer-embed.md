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
`viewer::Config::embed`. In embed mode the viewer:

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
| `nh:ready` | **exactly once**, after the scene build finished, its meshes were uploaded to the GPU, **and the first frame showing them has been rendered**. Not at runtime init and not when the build merely completes. | `{ type: 'nh:ready', camera }` — the pose that first frame was drawn with (after auto-framing and the URL camera, if any). |
| `nh:camera` | whenever the pose differs from the last one posted, throttled to **10 Hz** (one post per 100 ms at most; the final pose after a gesture is always delivered). Also immediately in reply to `nh:getCamera`. Never before `nh:ready`. | `{ type: 'nh:camera', camera }` |

### Host → viewer

| type | effect |
|---|---|
| `{ type: 'nh:setCamera', camera, animate? }` | Set the pose. `camera` must carry all six numeric fields (`targetX, targetY, targetZ, distance > 0, yawDeg, pitchDeg`); `projection` is optional. With `animate: true` the viewer eases from the current pose over ~0.4 s (smoothstep; shortest yaw arc; distance in log space) and user input cancels the ease. A `setCamera` that arrives before `nh:ready` becomes the initial camera applied after auto-framing. Messages failing validation are ignored with a console warning. |
| `{ type: 'nh:getCamera' }` | Reply with one `nh:camera` (no-op before `nh:ready`, whose payload already carries the pose). |

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
`nh:setCamera`:

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
    if (ev.data.type === 'nh:ready') {
      applyPoseToPreview(ev.data.camera);   // make both sides agree, then fade
      iframe.style.opacity = '1';
    } else if (ev.data.type === 'nh:camera') {
      applyPoseToPreview(ev.data.camera);   // keep the preview in step (optional)
    }
  });

  // Later: drive the viewer from the host.
  iframe.contentWindow.postMessage({
    type: 'nh:setCamera', animate: true,
    camera: { targetX: 0, targetY: 0, targetZ: 0, distance: 800,
              yawDeg: -45, pitchDeg: 10, projection: 'perspective' },
  }, '*');
  iframe.contentWindow.postMessage({ type: 'nh:getCamera' }, '*');
</script>
```

Hand the iframe the same camera on its URL that the preview is showing, so
the `nh:ready` fade reveals only added detail. Because `nh:ready` fires after
the first frame containing the scene has been *rendered*, there is no flash of
empty canvas when the opacity transition starts.

## Implementation map

| concern | where |
|---|---|
| option plumbing | `web/index.html` (`?embed`, `defaults.embed`) → `src/web/viewer_main.cpp` → `viewer::Config::embed` |
| UI / URL / storage gating | `src/viewer/app.cpp` (`cfg.embed` checks in `onFrame`, `savePersistentState`, `load*State`, `webAppModeProject`, `syncBrowserUrl`, `tickUrlSync`) |
| ready / camera posting, host camera, animation | `App::Impl::tickEmbedReady`, `tickEmbedCameraPost`, `setCameraFromHost`, `tickEmbedCameraAnim` in `src/viewer/app.cpp` |
| postMessage glue (EM_JS + C exports) | `src/viewer/platform_web.cpp` (`nh_viewer_embed_install`, `nh_viewer_embed_post`, `nh_viewer_embed_set_camera`, `nh_viewer_embed_request_camera`); native no-ops in `platform_native_default.cpp` / `platform_macos.mm` |
| camera JSON vocabulary | `src/viewer/camera_json.hpp` (GPU-free, in `nodehammer_lib`; tests in `tests/viewer/test_camera_json.cpp`) |
