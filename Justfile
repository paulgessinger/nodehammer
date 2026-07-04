# Vendored Conan recipes:
#   - manifold: patched to ship install/export rules under Emscripten.
#   - sokol-shdc: wraps the prebuilt shader compiler binary from
#     floooh/sokol-tools-bin (used as a tool_requires when viewer=True).
# Cheap to re-export every time; conan dedupes by recipe revision.
recipes:
    conan export recipes/manifold --version=3.2.1
    conan export recipes/sokol --version=2026.04.25
    conan export recipes/imgui --version=1.92.0
    conan export recipes/implot --version=0.17.0
    conan export recipes/nfd --version=1.2.1
    conan export recipes/sokol-shdc --version=2026.04.25

deps: recipes
    conan install . -s build_type=RelWithDebInfo --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja -o '&:viewer=True'

configure:
    cmake --preset conan-relwithdebinfo --fresh \
        -GNinja \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DNODEHAMMER_BUILD_TESTS=ON \
        -DNODEHAMMER_WITH_VIEWER=ON

build:
    cmake --build --preset conan-relwithdebinfo

test:
    ctest --preset conan-relwithdebinfo --output-on-failure -j14

configure-full:
    #!/bin/bash
    . /Users/pagessin/spack/share/spack/setup-env.sh
    spack env activate nodehammer-dev
    cmake \
        -GNinja \
        -S . \
        -B build \
        -DNODEHAMMER_WITH_TGEO=1 \
        -DNODEHAMMER_WITH_DD4HEP=1 \
        -DNODEHAMMER_BUILD_TESTS=1 \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        --fresh \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo


_convert file:
    build/RelWithDebInfo/nodehammer convert -i odd.nhb.zst -c fixtures/configs/{{file}}.toml -o build/{{file}}.glb --timing

ecal barrel: (_convert "odd_single_ecal_barrel_stave")
hcal barrel: (_convert "odd_single_hcal_barrel_stave")

odd: (_convert "odd")

# Wraps build/RelWithDebInfo/nodehammer; pass a subcommand and flags after `cli`.
# Not named `run`: `just run <recipe>` is built-in, and `just run x y` runs two recipes.
cli *args:
    #!/usr/bin/env bash
    set -euo pipefail
    exec "{{justfile_directory()}}/build/RelWithDebInfo/nodehammer" {{args}}

# ── Emscripten / wasm ────────────────────────────────────────────────────────
# Prereqs (one-time):
#   git clone https://github.com/emscripten-core/emsdk ~/emsdk
#   ~/emsdk/emsdk install latest && ~/emsdk/emsdk activate latest
# Each shell session:
#   source ~/emsdk/emsdk_env.sh  # exports EMSDK and puts em++ on PATH
#
# flatc comes in via conan's tool_requires (built for the native build profile),
# so no separate `brew install flatbuffers` is needed.

_require-emsdk:
    #!/usr/bin/env bash
    if [ -z "${EMSDK:-}" ]; then
        echo "EMSDK is not set. Run: source ~/emsdk/emsdk_env.sh" >&2
        exit 1
    fi

wasm-deps: _require-emsdk recipes
    conan install . \
        -pr:h profiles/emscripten \
        -pr:b default \
        -s build_type=RelWithDebInfo \
        -c tools.cmake.cmake_layout:build_folder_vars='["settings.os"]' \
        --build=missing \
        -o '&:viewer=True'

wasm-configure *args: _require-emsdk
    cmake --preset conan-emscripten-relwithdebinfo --fresh \
        -GNinja \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DNODEHAMMER_WITH_VIEWER=ON \
        -DNODEHAMMER_BUILD_TESTS=ON {{args}}

wasm-build: _require-emsdk
    cmake --build --preset conan-emscripten-relwithdebinfo

wasm-test: _require-emsdk
    ctest --preset conan-emscripten-relwithdebinfo --output-on-failure

# Layer additional cmake cache variables onto the wasm build without --fresh.
# Uses --preset so the emscripten toolchain/env stay in effect even if the
# cache is new. Follow up with `just wasm-build`.
# Example: just wasm-set -DNODEHAMMER_PROFILING=ON
wasm-set *args: _require-emsdk
    cmake --preset conan-emscripten-relwithdebinfo {{args}}

wasm: wasm-deps wasm-configure wasm-build wasm-test

# Release wasm build: -Oz + LTO + closure + assertions stripped. Slower link,
# smaller .wasm/.js, harder-to-read browser stack traces. Use for shipping.
wasm-deps-release: _require-emsdk recipes
    conan install . \
        -pr:h profiles/emscripten \
        -pr:b default \
        -s build_type=Release \
        -c tools.cmake.cmake_layout:build_folder_vars='["settings.os"]' \
        --build=missing \
        -o '&:viewer=True'

wasm-configure-release *args: _require-emsdk
    cmake --preset conan-emscripten-release --fresh \
        -GNinja \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DNODEHAMMER_WITH_VIEWER=ON \
        -DNODEHAMMER_BUILD_TESTS=ON {{args}}

wasm-build-release: _require-emsdk
    cmake --build --preset conan-emscripten-release

wasm-release: wasm-deps-release wasm-configure-release wasm-build-release

# Serve the wasm viewer locally. Browsers refuse to load .wasm from file://,
# so we need a real http server. The static viewer.html shell probes
# navigator.gpu and dynamically loads either nodehammer-gles3.{js,wasm}
# (WebGL2) or nodehammer-wgpu.{js,wasm} (WebGPU).
#
# The shell decides between URL-auto-load and upload-only mode by fetching
# a sibling `nh_manifest.json` at startup:
#   - If present, its `input` / `config` fields name the scene to load.
#   - If 404, the viewer comes up empty and accepts drag-and-drop /
#     file-picker uploads.
#
# Usage:
#   just wasm-serve                 # ODD scene, autoload (default)
#   just wasm-serve odd             # explicit ODD scene, autoload
#   just wasm-serve odd_simple      # simplified ODD scene, autoload
#   just wasm-serve none            # no manifest; upload-only deployment
#   just wasm-serve odd 9000        # ODD scene on a custom port
wasm-serve scene='odd' port='8000':
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    dir="$root/build/emscripten/RelWithDebInfo"
    mapfile -t staged < <("$root/scripts/stage_wasm_viewer.sh" link "{{scene}}" "$dir")
    mode="${staged[1]}"

    echo "serving $dir in $mode at:"
    echo "  http://localhost:{{port}}/viewer.html"
    cd "$dir" && exec python3 -m http.server {{port}}

# Copy the same static viewer payload that `wasm-serve` exposes into a
# self-contained directory, sourced from the Release wasm build. The target
# may be relative to the repo root or absolute. Build the Release wasm first
# with `just wasm-release`.
#
# Usage:
#   just wasm-copy                              # copy ODD autoload bundle to build/wasm-viewer
#   just wasm-copy odd public/viewer            # copy ODD autoload bundle to a custom directory
#   just wasm-copy odd_simple public/viewer     # copy simplified ODD autoload bundle
#   just wasm-copy none public/viewer           # copy upload-only bundle
wasm-copy scene='odd' target='build/wasm-viewer':
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    src="$root/build/emscripten/Release"
    mapfile -t staged < <("$root/scripts/stage_wasm_viewer.sh" copy "{{scene}}" "{{target}}" "$src")
    out="${staged[0]}"
    mode="${staged[1]}"

    echo "copied wasm viewer payload to $out in $mode"

# Headless smoke for the compute-worker module: generates a small synthetic
# semantic scene with the native build, then drives nh_compute_build in node via
# the wasm compute module, asserting NHR8 render bytes come back. Build both
# first: `cmake --build build/RelWithDebInfo --target nodehammer` and
# `just wasm-build`.
wasm-compute-smoke:
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    native="$root/build/RelWithDebInfo/nodehammer"
    module="$root/build/emscripten/RelWithDebInfo/nodehammer-compute.js"
    fixture="$(mktemp -t nh_compute_smoke.XXXXXX.nhb)"
    trap 'rm -f "$fixture"' EXIT
    "$native" dump-semantic --input dummy --input-format synthetic --output-format nhb -o "$fixture"
    node "$root/scripts/wasm_compute_smoke.js" "$module" "$fixture"
