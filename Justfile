deps:
    conan install . -s build_type=RelWithDebInfo --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja

configure:
    # Avoid -B: it overrides the preset binaryDir from Conan cmake_layout.
    cmake --preset conan-relwithdebinfo --fresh \
        -GNinja \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build:
    cmake --build --preset conan-relwithdebinfo

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

wasm-deps: _require-emsdk
    conan install . \
        -pr:h profiles/emscripten \
        -pr:b default \
        -s build_type=RelWithDebInfo \
        -c tools.cmake.cmake_layout:build_folder_vars='["settings.os"]' \
        --build=missing

wasm-configure *args: _require-emsdk
    cmake --preset conan-emscripten-relwithdebinfo --fresh \
        -GNinja \
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
