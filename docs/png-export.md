# High-resolution PNG export (viewer screenshots)

Status: native/Metal path landed and verified. Backend-based readback in place
for Metal (verified), WebGL2/GLES3 and WebGPU (both compile + link in the wasm
build; browser runtime verification still pending). D3D11 is a build-time
`#error` stub.

## Goal

Let the viewer export a PNG of the current view. The export dials every quality
knob to maximum, renders the frame at a high (supersampled) resolution, then
scales it down to a configurable output size and writes/downloads a PNG. Works
on both native and web.

## Design

SSAA pipeline — render big, average down:

1. **Render** the scene at `output × supersample` with a maxed quality snapshot
   (FXAA Ultra, AO Ultra, full-res AO, HDR, no dynamic scale, no pause). The
   output dimensions also drive the camera aspect ratio for the exported frame,
   independent of the live window.
2. **Composite** that converged frame into a dedicated offscreen LDR target
   (`export_out_rt`), using the swapchain's color/depth formats so the existing
   composite pipeline validates against it.
3. **Read back** the LDR target GPU→CPU.
4. **Downscale** by the supersample factor (exact integer box filter) and
   **encode** PNG (vendored `stb_image_write`).
5. **Deliver**: native writes to the working directory; web triggers a browser
   download.

Only two things are platform/backend specific: **GPU readback** (per graphics
backend) and **file delivery** (write vs. download). Everything else is shared.

**WebGPU `CopySrc` caveat.** WebGPU can't `copyTextureToBuffer` from a texture
unless it was created with the `CopySrc` usage — and sokol never sets that on
render targets (it hardcodes `TextureBinding | CopyDst | RenderAttachment`).
There's no `sg_image_usage` flag for it either. So `SceneRenderTarget::create`
takes a `for_readback` flag, and when set it asks the active backend for the
color image via `makeReadbackColorImage` (declared in `png_export_readback.hpp`,
one definition per readback TU). Metal/GL return `SG_INVALID_ID` (any color
attachment is readable as-is, so the caller allocates it normally); the WebGPU
TU injects a self-created `WGPUTexture` (with `CopySrc`) through
`sg_image_desc.wgpu_texture`, dropping its own ref afterwards so the texture is
freed with the `sg_image`.

### Where it lives

- `include/nodehammer/viewer/png_export.hpp` — `PngExportSettings`
  (`out_width`, `out_height`, `supersample`) + `downscaleBoxRgba8` /
  `encodePngRgba8` helpers.
- `src/viewer/png_export.cpp` — downscale + encode (no GPU).
- `src/viewer/stb_image_write_impl.cpp` — the single `STB_IMAGE_WRITE_IMPLEMENTATION` TU.
- `third_party/stb/stb_image_write.h` — vendored (Conan's tinygltf ships only
  `tiny_gltf.h`); excluded from clang-format.
- `src/viewer/png_export_readback.hpp` — `ImageReadback` interface
  (`begin` / `poll` / `reset`, `ReadbackStatus`). Returns tightly-packed RGBA8,
  top-left origin (each backend handles BGRA→RGBA swizzle and bottom-left flips).
  Also declares `makeReadbackColorImage` — a per-backend hook for creating the
  export target's color image (see the WebGPU `CopySrc` note below).
- `src/viewer/png_export_readback_<backend>.{cpp,mm}` — one impl per graphics
  backend, selected in CMake by the `SOKOL_<backend>` being compiled.
- `src/viewer/app.cpp` — export state machine + render integration.
- `src/viewer/ui/{view_panel,menu_bar,ui_context}.*` — UI + action wiring.
- `src/viewer/platform*.{cpp,mm}` + `include/nodehammer/viewer/platform.hpp` —
  `saveExportedImage` (native file write / web download).
- `src/cli/cmd_viewer.cpp` — headless `--screenshot` CLI mode.

### Export state machine (driven from `App::Impl::onFrame`)

`Idle → Rendering → WaitGpu → Readback → Idle`

- **Rendering**: reuse the live scene/AO pipeline, but `render()` overrides the
  target dims to the export resolution and swaps in the maxed quality snapshot
  for the frame (restored before returning). Render ~8 frames so the GTAO
  temporal denoise + frame-late AO history converge; on the last one, also
  composite into `export_out_rt`.
- **WaitGpu**: a few normal frames so the capture frame's GPU work drains before
  readback (on Metal a later `sg_begin_pass` blocks on the in-flight semaphore,
  guaranteeing completion).
- **Readback**: `ImageReadback::begin` then `poll` until `Ready`/`Failed`, then
  downscale + encode + deliver.

A pending export keeps the frame loop awake (bypasses the idle / pause /
fps-cap gates), otherwise an unfocused window would park before the trigger.

### Backend-based readback selection

Readback is chosen by the **compiled `SOKOL_<backend>`**, not by web-vs-native —
WebGPU (Dawn) and desktop GL can run natively too. `nh_add_viewer_lib(name
sokol_lib backend)` maps:

| Backend          | TU                                  | Notes                                |
|------------------|-------------------------------------|--------------------------------------|
| `SOKOL_METAL`    | `png_export_readback_metal.mm`      | Done + verified.                     |
| `SOKOL_WGPU`     | `png_export_readback_wgpu.cpp`      | Done (compiles/links); browser verify pending. `copyTextureToBuffer` + async map. |
| `SOKOL_GLES3`    | `png_export_readback_gl.cpp`        | Done (compiles/links); browser verify pending. FBO + `glReadPixels` (flip rows). |
| `SOKOL_GLCORE`   | `png_export_readback_gl.cpp`        | `#error` — needs a desktop GL loader. |
| `SOKOL_D3D11`    | `png_export_readback_d3d11.cpp`     | `#error` stub for now.               |

sokol exposes the native handles needed for each:
`sg_mtl_device` / `sg_mtl_query_image_info`,
`sg_wgpu_device` / `sg_wgpu_queue` / `sg_wgpu_query_image_info`,
`sg_gl_query_image_info` (`tex[]`, `tex_target`, `active_slot`).

## Done

- **Phase 1 — native/Metal end-to-end** (committed, `Add high-res PNG screenshot
  export (native/Metal)`):
  - Settings struct, downscale + stb PNG encode, vendored stb + REUSE entry.
  - `ImageReadback` interface + Metal impl (blit render-target texture to a
    shared `MTLBuffer`, swizzle BGRA→RGBA).
  - Export state machine + `render()` integration (dims/quality override, extra
    composite into `export_out_rt`, idle-gate bypass).
  - UI: width / height / supersample + "Export PNG" in the View panel and File
    menu, progress toast.
  - Platform delivery: native cwd write; web `nh_viewer_download_bytes` download.
  - Headless `viewer --screenshot <path> [--screenshot-width/-height/-supersample]`
    CLI mode (renders one PNG on startup, then quits).
  - Verified: `nodehammer viewer --input odd.nhb.zst --config odd_flat.toml
    --screenshot out.png` produces a correct ODD render at the requested size and
    aspect (tested 1280×720 @2× and 800×600 @3×). All 335 unit tests pass.

- **Phase 2 — backend-based readback restructure + GL/WebGPU TUs**
  (CMake restructure committed in `WIP png exporrt`; TUs uncommitted):
  - `nh_add_viewer_lib` takes the backend and selects the readback TU by
    `SOKOL_<backend>`; call sites in the root `CMakeLists.txt` updated. Removed
    the platform-named `png_export_readback_web.cpp` / `_native_other.cpp` stubs.
  - `png_export_readback_gl.cpp` — scratch FBO + `glReadPixels`, rows flipped for
    the top-left contract. No swizzle: `glReadPixels(GL_RGBA)` returns *logical*
    RGBA regardless of internal byte order. Synchronous (like Metal). `#error`
    on `SOKOL_GLCORE` (needs a desktop GL function loader, not wired).
  - `png_export_readback_wgpu.cpp` — `copyTextureToBuffer` into a
    `MapRead|CopyDst` buffer (256-byte `bytesPerRow` align), submitted on
    `sg_wgpu_queue()`, then `wgpuBufferMapAsync` with
    `WGPUCallbackMode_AllowSpontaneous` polled across frames (`poll` returns
    `Pending` until the map callback flips a flag; the browser/Dawn event loop
    resolves it between render frames — no `processEvents` needed). De-pads rows
    + swizzles BGRA→RGBA on copy-out. Uses the modern future-based emdawnwebgpu
    signature (`WGPUBufferMapCallbackInfo`, `WGPUTexelCopy*Info`).
  - `makeReadbackColorImage` hook + `SceneRenderTarget::create(..., for_readback)`
    so the WebGPU export target gets a `CopySrc` texture (see the caveat above).
  - `png_export_readback_d3d11.cpp` — `#error` stub (no Windows env to test).
  - Verified: native Metal screenshot still correct after the shared-file
    changes; the emscripten build configures and both `nodehammer-{gles3,wgpu}`
    bundles compile + link with their readback TUs.

## Next

1. **Verify web in a browser** (`just wasm-serve`) — export from both the gles3
   and wgpu bundles and confirm the downloaded PNG is correct (not blank /
   wrong-channel / vertically flipped). Watch the WebGPU path in particular: the
   `AllowSpontaneous` map callback firing between frames is the one thing that
   couldn't be checked at compile time.
2. Commit Phase 2 (optionally fold the `WIP png exporrt` commit into it).

## Open questions / possible follow-ups

- Transparent-background export (alpha PNG) vs. the current opaque clear + IBL
  dome.
- 16-bit / EXR HDR output (current export is 8-bit PNG matching the screen).
- A native "Save As…" dialog (currently writes a timestamped file to the cwd).
- Letterbox the live window during an export when the output aspect differs from
  the window (currently the in-progress preview can look stretched for a few
  frames; the exported file is always correct).
