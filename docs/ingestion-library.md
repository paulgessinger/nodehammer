# Integrating nodehammer

The C++20 public API uses explicit capability headers such as
`<nodehammer/nhb.hpp>` and `<nodehammer/tgeo.hpp>`. See the
[public API guide](public-api-sketch.md) for the complete surface. Choose the library that provides the
capabilities your application needs; there is no implementation macro or
amalgamated header.

## Installed SDK components

Both SDK configurations install the same `nodehammer` CMake package:

| Installation | Available components and targets |
| --- | --- |
| `NODEHAMMER_INGEST_ONLY=ON` | `Ingest` → `nodehammer::ingest` |
| Full build with `NODEHAMMER_BUILD_SHARED=ON` | `Ingest` → `nodehammer::ingest`, `Shared` → `nodehammer::shared` |
| Default full build without the shared library | Executable only; no installed SDK |

Select the minimal shared library with:

```cmake
find_package(nodehammer CONFIG REQUIRED COMPONENTS Ingest)
target_link_libraries(my_experiment PRIVATE nodehammer::ingest)
```

Or select the full shared library with:

```cmake
find_package(nodehammer CONFIG REQUIRED COMPONENTS Shared)
target_link_libraries(my_application PRIVATE nodehammer::shared)
```

With no explicit components, `find_package(nodehammer CONFIG REQUIRED)` loads
all components available in that installation. Requesting a required component
that was not installed fails with a message naming it. Loading both targets
does not link both: choose one for each consumer. The full shared library
already contains ingestion. The older `nodehammer::nodehammer` spelling remains
an alias for `nodehammer::shared`.

The `Ingest` component provides in-memory ROOT/DD4hep geometry traversal,
scene counts, diagnostics, and raw or zstd-compressed `.nhb` reading and writing. It reuses
existing backend installations; ROOT and DD4hep are never downloaded. GeoModel
and event transport are future additions. File I/O and format discovery in `io.hpp`
require the full shared library, as do
configuration, tessellation, other import/export formats, and the callable CLI.

```cpp
#include <nodehammer/dd4hep.hpp>
#include <nodehammer/nhb.hpp>

auto result = nodehammer::fromDD4hep(detector); // dd4hep::Detector&
// Or: fromTGeo(manager), where manager is a TGeoManager&.
auto bytes = nodehammer::toNhbZstd(result.scene); // or toNhb(result.scene) for uncompressed bytes
auto restored = nodehammer::fromNhb(bytes);
// Inspect result.diags; save bytes or send them through the experiment's transport.
```

The caller retains ownership of the input geometry. Both shared libraries absorb
static zstd and expose no implementation dependencies through their CMake
interfaces. Enabled ROOT/DD4hep backends remain shared runtime dependencies.
Applications that themselves use ROOT/DD4hep APIs resolve and link those
packages explicitly. For example, an application constructing a TGeoManager
links both `nodehammer::ingest` and `ROOT::Geom`.

Link one nodehammer library in a consumer: the full shared library already
contains the ingestion implementation. Passing handles between independently
loaded ingestion and full libraries is not a supported integration boundary;
use serialized bytes when those deployments need to exchange geometry.

## FetchContent and source embedding

Build options select what is configured when consuming the source:

```cmake
include(FetchContent)

find_package(ROOT REQUIRED COMPONENTS Geom)
find_package(DD4hep REQUIRED)

set(NODEHAMMER_INGEST_ONLY ON)
set(NODEHAMMER_WITH_DD4HEP ON) # also enables TGeo
set(NODEHAMMER_BUILD_TESTS OFF)

FetchContent_Declare(nodehammer
    GIT_REPOSITORY https://github.com/paulgessinger/nodehammer.git
    GIT_TAG codex/ingestion-library # development branch; pin a commit in production
)
FetchContent_MakeAvailable(nodehammer)
target_link_libraries(my_experiment PRIVATE nodehammer::ingest DD4hep::DDCore)
```

For ROOT alone, omit the DD4hep lookup and set `NODEHAMMER_WITH_TGEO ON` instead.
`add_subdirectory(external/nodehammer)` can replace FetchContent when the source
is already present. For a full shared source build, set `NODEHAMMER_INGEST_ONLY
OFF` and `NODEHAMMER_BUILD_SHARED ON`, then link `nodehammer::shared`.

The minimal implementation uses glm, unordered_dense, FlatBuffers, and zstd. CMake
reuses existing targets or compatible installed packages, falling back to
FetchContent. zstd is built as a position-independent static library and absorbed
into the ingestion shared library; it is not a separately shipped dependency.
Fetching FlatBuffers also builds `flatc`; an installed dependency
must provide a compatible compiler target or host `flatc` on PATH. The semantic
schema header is generated in the build directory, never committed or required
by installed consumers. Cross-compilation requires a compatible host `flatc`.
Standard `FETCHCONTENT_SOURCE_DIR_<NAME>` overrides support local dependency
sources and offline builds. The ingestion-only path never configures Manifold,
the renderer, Lua, CLI11, or the full processing pipeline.

## Building and installing

Minimal SDK:

```sh
cmake -S . -B build-ingest -G Ninja \
  -DNODEHAMMER_INGEST_ONLY=ON -DNODEHAMMER_WITH_TGEO=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-ingest
cmake --install build-ingest --prefix /path/to/install
```

Full SDK plus executable:

```sh
cmake -S . -B build-full -G Ninja \
  -DNODEHAMMER_BUILD_SHARED=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-full
cmake --install build-full --prefix /path/to/install
```

The full SDK installs the ingestion shared library, full shared library, executable,
public headers, and CMake package. Backend adapter headers always exist; disabled backends provide `#error` stubs
with rebuild instructions. The minimal SDK contains no full-library I/O, config, or render
headers. It does not install the full static archive.
CMake **install components** are distinct from `find_package` components:
`Development` contains headers, exports, and shared link files; `Runtime`
contains the shared libraries and, for full builds, the executable. Both SDK
configurations need Runtime and Development. An unfiltered
`cmake --install` installs all available components.

Use a compatible compiler, C++ runtime, and build configuration, especially on
Windows. Backend selection is shared across targets in one build directory;
different selections require separate builds.

## Compilation and Python packaging

Ingestion translation units compile as C++20 into an object library. On
macOS/Linux, those objects are reused in the ingestion shared library, the internal
complete `nodehammer_lib` archive, and `nodehammer_shared`. Remaining library
sources compile as C++23. Windows builds separate static and DLL ingestion
objects because their export annotations differ.

The executable links the full static archive. That archive contains nodehammer's
own objects, not a bundle of every third-party archive; CMake supplies those
additional dependencies at the final link. It is not an installed SDK target.

Python links the full shared library. Wheel builds select the Python build
target and install component, packaging the extension and its sibling shared
library without the SDK, standalone executable, or static archives. They do not
build the unused static variant.

`ci/verify-ingest.sh` tests standalone installation and source embedding from a
separate C++20 project. Both SDK install checks exercise component selection,
default loading, optional/missing components, and dependency isolation. The
geometry tests check nested world transforms directly; the full ROOT-enabled
build also compares `.nhb` bytes against the ingestion shared library.
