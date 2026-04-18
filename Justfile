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
