# nodehammer

**A HEP geometry conversion pipeline — and a real-time, physically-based viewer to see what you built.**

`nodehammer` takes detector geometry from High Energy Physics toolchains (ROOT/TGeo, DD4hep, GDML/Geant4, GeoModel) and turns it into clean, semantically-tagged, renderable scenes — then lets you fly through them at 60fps with GPU ambient occlusion, HDR tonemapping, and a procedural sky, on desktop *and* in the browser.

---

## Why

Detector geometries are huge, deeply nested, and encoded in formats built for simulation — not for looking at. nodehammer bridges that gap:

```
TGeo / DD4hep / GDML / GeoModel
              │
              ▼
    Semantic scene graph (.nhb / .nhb.zst)
    nodes · shapes · materials · deduplicated transforms
              │
       ┌──────┴──────┐
       ▼             ▼
  glTF / OBJ    Render IR (NHR8, FlatBuffers)
                       │
                       ▼
              Interactive viewer
   native (Metal / D3D11 / GLCORE) · web (WebGPU / GLES3, wasm)
```

Every stage is driven by declarative TOML config — `keep_if` / `drop_if` selection rules with a small predicate-expression language (`tag.sensitive == "true" && any(path ~= "**/Pixels/**")`), material remapping, and boolean cuts — so you can go from "the whole detector" to "just the Pixel barrel" without touching code.

## Highlights

- **Multiple importers** — TGeo/ROOT, DD4hep, GDML/Geant4, GeoModel, each independently switchable at build time.
- **A real intermediate format** — `.nhb`/`.nhb.zst`: a compact, zstd-compressible FlatBuffers container for the full semantic scene graph, with a documented [on-disk spec](docs/nhb-format.txt).
- **Analytical geometry surgery** — semantic-level azimuthal wedge cuts and boolean operations (via [manifold](https://github.com/elalish/manifold)) that stay correct on non-manifold swept primitives.
- **A viewer that actually looks good**:
  - Full offscreen HDR pipeline — ACES / Reinhard / AgX tonemapping, exposure control
  - GTAO screen-space ambient occlusion
  - Nishita single-scattering atmospheric sky (real Rayleigh + Mie, a real sun disc) baked straight into the IBL pipeline
  - FXAA 3.11 console-quality antialiasing
  - Dynamic, GPU-load-driven render scale with a power-saving idle profile — renders on demand, caps idle frame rate
  - High-resolution, supersampled PNG screenshot export with backend-specific GPU readback (Metal, GL/GLES3, WebGPU) — `nodehammer viewer --screenshot out.png --screenshot-width ... --screenshot-supersample ...`
  - Live ImPlot performance graphs in the debug panel
- **Runs everywhere** — native builds on Metal (Apple), D3D11 (Windows), or GLCORE (Linux); a wasm build targets both GLES3 and WebGPU from one configure, with the browser shell auto-picking the right bundle via `navigator.gpu`. A headless compute-worker wasm module (`nodehammer-compute`) runs tessellation and boolean cuts off the main thread.
- **Modern C++** — C++23 throughout (`<print>`, the works); requires GCC ≥ 14, Clang ≥ 18/libc++, or MSVC ≥ 19.37.

## CLI

```
nodehammer convert         # geometry → glTF / render IR
nodehammer inspect         # explore a semantic scene
nodehammer validate-config # lint a TOML config before running it
nodehammer config-flatten  # resolve config includes/overrides
nodehammer dump-semantic   # dump the semantic scene as JSON
nodehammer dump-render     # dump the render IR as JSON
nodehammer viewer          # launch the interactive viewer
```

## Building

Dependencies are managed with [Conan](https://conan.io/); builds with CMake + Ninja.

```bash
just recipes    # export vendored recipes (manifold, sokol-shdc)
just deps       # conan install (viewer=True)
just configure  # cmake --preset conan-relwithdebinfo
just build
just test
```

Or drive `conan`/`cmake` directly — see the [Justfile](Justfile) for the exact flags, including the Emscripten/wasm targets (`just wasm-deps`, requires [emsdk](https://github.com/emscripten-core/emsdk)) and the full spack-based dev configuration (`configure-full`) that enables every importer at once.

Key CMake options:

| Option | Purpose |
|---|---|
| `NODEHAMMER_WITH_VIEWER` | Build the sokol/Dear ImGui interactive viewer |
| `NODEHAMMER_WITH_TGEO` | ROOT/TGeo importer |
| `NODEHAMMER_WITH_DD4HEP` | DD4hep importer |
| `NODEHAMMER_WITH_GEANT4` | GDML/Geant4 importer |
| `NODEHAMMER_WITH_GEOMODEL` | GeoModel importer |
| `NODEHAMMER_BUILD_TESTS` | Build the Catch2 unit-test binary |

## Try it

The repo ships a real detector fixture — the Open Data Detector — ready to convert:

```bash
just odd    # full ODD → glb (see the Justfile for single-stave recipes too)
build/RelWithDebInfo/nodehammer viewer
```

## Project layout

```
src/           core pipeline: cli, config, ir (intermediate representation),
               scene building, tessellation, selection, the viewer, web glue
shaders/       sokol-shdc GLSL sources for the full render pipeline
schemas/       FlatBuffers schemas (render.fbs, semantic.fbs)
profiles/      Conan profiles (Emscripten cross-compilation)
recipes/       vendored/patched Conan recipes (manifold, sokol-shdc)
fixtures/      sample detector geometries and configs
docs/          format specs and design docs (nhb format, predicate expressions,
               orbit navigation, rendering-fidelity strategy, PNG export)
web/           browser viewer entry point
```

## License

MIT © Paul Gessinger — see [LICENSE](LICENSE). Third-party license texts for vendored/bundled assets live under [LICENSES/](LICENSES).
