# Public C++ API

All public types and functions live directly in `nodehammer`. Headers use C++20;
the full implementation builds as C++23. Public headers expose standard-library
types and opaque scene/configuration handles, not implementation dependencies.

## Geometry handles

`semantic_scene.hpp` declares `SemanticScene` and `SemanticResult` (`scene`,
`diags`). `render_scene.hpp` declares `RenderScene` and `RenderResult`.
Handles are immutable and cheap to copy. Their members inspect validity and
counts; conversion and file operations are free functions.

## In-memory conversion

| Header | Functions |
| --- | --- |
| `nhb.hpp` | `fromNhb(bytes)`, `toNhb(scene)`, `toNhbZstd(scene, level = 3)` |
| `nhr.hpp` | `fromNhr(bytes)`, `toNhr(scene)` |
| `tgeo.hpp` | `fromTGeo(TGeoManager&)` |
| `dd4hep.hpp` | `fromDD4hep(dd4hep::Detector&)` |

Backend headers are exposed only when that backend is enabled. Inputs remain
caller-owned; imported scenes own their representation independently. NHB/NHR
readers accept raw or zstd-compressed bytes. Semantic imports return
`SemanticResult`; NHR decoding returns `RenderScene` directly.

```cpp
#include <nodehammer/tgeo.hpp>
#include <nodehammer/nhb.hpp>

auto [scene, diagnostics] = nodehammer::fromTGeo(manager);
auto bytes = nodehammer::toNhbZstd(scene);
auto restored = nodehammer::fromNhb(bytes);
```

## File I/O

`io.hpp` declares:

```cpp
SemanticResult readSemantic(const std::filesystem::path&, const SemanticReadOptions& = {});
RenderScene readRender(const std::filesystem::path&);
void write(const SemanticScene&, const std::filesystem::path&, const SemanticWriteOptions& = {});
void write(const RenderScene&, const std::filesystem::path&, const OutputConfig& = {},
           const RenderWriteOptions& = {});
```

Read options contain `format`; write options contain `format` and
`compressionLevel` (default 3). Empty format means filename inference. A `.zst`
suffix selects compression independently of the format override. The level only
applies when compression is selected. Unsupported compressed output formats throw.

`semanticReadFormats`, `semanticWriteFormats`, `renderReadFormats`, and
`renderWriteFormats` return views over library-lifetime lists of supported names.
Reading render scenes currently supports NHR only.

```cpp
nodehammer::write(scene, "geometry.nhb.zst", {.compressionLevel = 9});
```

## Configuration and processing

`config.hpp` provides `readConfig(path)`, `fromToml(text, baseDir = {})`,
`checkConfig(path)`, `checkConfigString(text, baseDir = {})`, and `configFormats()`.
An empty base directory means the text has no location; it does not select the
working directory. Successful loading returns `ConfigResult` (`config`, `diags`).
`Config` exposes `scene()`, `output()`, and `valid()`. Default `SceneConfig` and
`OutputConfig` use built-in defaults.

`build.hpp` declares `applySelection`, `deduplicate`, `tessellate`, and `build`.
Each takes a semantic scene and scene configuration. `build` runs selection,
deduplication, and tessellation in order.

```cpp
#include <nodehammer/build.hpp>
#include <nodehammer/io.hpp>

auto geometry = nodehammer::readSemantic("detector.root");
auto config = nodehammer::readConfig("nodehammer.toml");
auto rendered = nodehammer::build(geometry.scene, config.config.scene());
nodehammer::write(rendered.scene, "detector.glb", config.config.output());
```

## Errors, CLI, and version

`diagnostics.hpp` provides `Diagnostic`, `DiagnosticList`, `hasErrors`, and
`Error`. Failures throw; returned diagnostics describe observations about an
existing result. See [error-model.md](error-model.md).

`cli.hpp` exposes `runCli(args, CliOptions{})`. It returns the CLI exit status;
arguments omit the program name. Defaults disable paging and enable quiet mode.
`version.hpp` exposes compile-time `VERSION` constants and the linked `version()`.

## Python

Python mirrors the free functions in snake_case. Scene counts and configuration
slices remain properties. `read_config` accepts string/path-like filenames;
`from_toml` takes text and `from_dict` takes a dictionary. The explicit function
name determines whether a value is a path or document content.

```python
import nodehammer as nh

scene, diagnostics = nh.read_semantic("detector.nhb.zst")
config, diagnostics = nh.from_toml("deduplicate_shapes = true")
rendered, diagnostics = nh.build(scene, config.scene)
nh.write(rendered, "detector.glb", config.output)
```

Migration: replace scene static reads with `readSemantic`/`readRender` for paths,
`fromNhb`/`fromNhr` for bytes, or `fromTGeo`/`fromDD4hep` for live geometry. Replace
scene serialization and write members with free functions taking the scene as
the first argument. Replace static `Config` loading/checking functions with the
namespace-level functions above. Replace `cli::run`/`cli::RunOptions` with
`runCli`/`CliOptions`. Rebuild clients: these are source and ABI changes.
