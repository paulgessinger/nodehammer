# Headless WASM compute profiling

The compute worker can run in Node without a browser or GPU. This exercises the
browser worker's `nh_compute_build` ABI: semantic FlatBuffer decoding, config
parsing, scene preparation, optional wedge cutting, tessellation, and render
serialization. ROOT/DD4hep ingestion, network transfers, and rendering are outside
this benchmark.

## Build

Activate Emscripten, resolve dependencies, then build the compute target with
profiling enabled:

```sh
source ~/emsdk/emsdk_env.sh
just wasm-deps
just wasm-configure -DNODEHAMMER_PROFILING=ON
cmake --build --preset conan-emscripten-relwithdebinfo --target nodehammer-compute
```

Emscripten requires Python >= 3.10. If the system Python is older, set
`EMSDK_PYTHON` to a suitable executable both before and after sourcing the SDK
(the activation script can clear it). Match the Conan host `compiler.version`
to the SDK's Clang major; `profiles/emscripten` is the CI default. A newer compiler
may also require updating Conan's supported settings. SDK/Conan/compiler caches
must be writable.

For an SDK reporting Clang 23, the dependency command with an explicit override is:

```sh
conan install . -pr:h profiles/emscripten -pr:b default \
  -s build_type=RelWithDebInfo -s:h compiler.version=23 \
  -c 'tools.cmake.cmake_layout:build_folder_vars=["settings.os"]' \
  --build=missing -o viewer=True
```

Export the vendored recipes with `just recipes` first. If Conan does not yet
recognize Clang 23, extend its `settings_user.yml` compiler versions before this
command; do not label a Clang 23 build as Clang 22.

`NODEHAMMER_PROFILING` preserves function names and enables the compute module's
local `nhComputeTiming` and `nhComputeDiagnostic` callbacks. Normal builds have
neither instrumentation nor additional worker messages.

## Run

Prepare uncompressed geometry and a flattened config using the native CLI:

```sh
mkdir -p build/wasm-profile
zstd -dc odd.nhb.zst > build/wasm-profile/odd.nhb
build/RelWithDebInfo/nodehammer config flatten \
  -c fixtures/configs/odd.toml -o build/wasm-profile/odd.toml
node scripts/wasm_compute_profile.js \
  build/emscripten/RelWithDebInfo/nodehammer-compute.js \
  build/wasm-profile/odd.nhb build/wasm-profile/odd.toml \
  build/wasm-profile/timings.json 0 90 3
```

The final arguments are wedge start/end degrees and number of cached repeats.
Each of the uncut and wedge scenarios uses its own module instance, one fresh
input load, and the requested cached rebuilds. The pristine scene/config cache
is reused; tessellation still runs on every call. A fresh load includes Wasm JIT
warmup effects and is not a statistically independent cold-start benchmark.

The harness rejects failed builds, error diagnostics, invalid render identifiers,
and output hashes that differ between fresh and cached builds. Warnings are
retained in JSON. It requires the profiling-enabled module.

To collect CPU samples, repeat the command with
`NH_CPU_PROFILE=build/wasm-profile/compute.cpuprofile` in its environment. Load the
result into a DevTools JavaScript CPU profile viewer. Use the separate run without
sampling for reported timings; sampling adds overhead.

## Interpret

- `initializationMs`: module instantiation, separate from conversion.
- `callMs`: the synchronous ABI call, including native result cleanup.
- `wallMs`: input allocation/copy, ABI call, and output copy; excludes file I/O,
  hash verification, and freeing caller-owned buffers.
- `stagesMs`: measured work around each pipeline advance, attributed to its entry
  phase. Wedge bounds/classification/pruning yield at their boundaries.
  `input_decode_config_cache` contains `decode` and `config`; do not sum all three.
  `total` includes the stages plus setup/cleanup and callback overhead.
- `wasmHeapCapacityBytes`: allocated linear-memory capacity after the build,
  **not** live bytes or peak allocation. `processRssBytes` covers the whole Node
  process and can include previous module instances awaiting garbage collection.

RelWithDebInfo uses `-O2`; shipping Release compiles the core with `-O3` and
uses `-Oz`/LTO at the compute-module compile/link step. For Release,
use `just wasm-deps-release`, `just wasm-configure-release -DNODEHAMMER_PROFILING=ON`,
and build `nodehammer-compute` with the `conan-emscripten-release` preset. Point
the harness at `build/emscripten/Release/nodehammer-compute.js`.
WASM profiling adds function names, not forced DWARF debug information: the
latter limits Binaryen optimization and would distort the Release baseline.
Node/V8 measurements describe compute cost on this host, not browser frame time.

## Measured optimization results

On 2026-10-14, the ODD fixture and flattened ODD config were measured with
Emscripten 5.0.6 / Clang 23, Node v23.11.1, and an AMD Ryzen Threadripper 3970X.
Both versions used Release with profiling function names, without forced DWARF.
CPU sampling ran separately from the timing measurements.

| Scenario | Baseline cached | Optimized cached | Time reduction |
|---|---:|---:|---:|
| Uncut | 5.94 s | 3.61 s | 39.2% |
| Wedge 0–90° | 7.79 s | 5.02 s | 35.6% |

Each scenario used one fresh load and three cached rebuilds; the table reports
cached medians. Fresh measurements were 4.63 s uncut and 5.82 s with the wedge
after optimization. These are local workload measurements, not a general
performance guarantee.

The retained changes specialize common glob patterns, reject anchored mismatches
early, reuse validated metadata route prefixes, initialize tree metadata in bulk,
avoid duplicate validation and unused world-transform calculations, and use dense
tables for immutable occurrence indexes and validation.

All render outputs remained byte-identical to the baseline: 18,764,008 bytes uncut
and 41,816,400 bytes with the wedge. Linear-memory capacity remained 667.31 MiB.
The complete WASM suite passed 557 test cases / 113,359 assertions, including an
independent exhaustive glob comparison, metadata identity/edit regressions, and
invalid canonical occurrence inputs.
