#!/usr/bin/env bash
# Shared CI helper used by .github/workflows/ci.yml (build matrix + LCG job).
#
# Dispatcher style so each CI step keeps its own log block while the
# kind → conan/cmake-preset mapping lives in one place.
#
# Usage:
#   ci/build.sh conan-install <kind>
#   ci/build.sh configure     <kind> [extra cmake args...]
#   ci/build.sh build         <kind>
#   ci/build.sh test          <kind> [extra ctest args...]
#   ci/build.sh install       <kind> <prefix>
#
# Kinds: native, wasm

set -euo pipefail

cmd=${1:?subcommand required: conan-install|configure|build|test}
kind=${2:?kind required: native|wasm}
shift 2

case "$kind" in
  native)
    conan_args=(-s:a compiler.cppstd=23 -c tools.cmake.cmaketoolchain:generator=Ninja)
    preset=conan-release
    build_dir=build/Release
    install_extra=(--strip)
    ;;
  wasm)
    conan_args=(
      -pr:h profiles/emscripten
      -pr:b default
      -c 'tools.cmake.cmake_layout:build_folder_vars=["settings.os"]'
    )
    preset=conan-emscripten-release
    build_dir=build/emscripten/Release
    install_extra=()
    ;;
  *)
    echo "unknown kind: $kind" >&2
    exit 1
    ;;
esac

case "$cmd" in
  conan-install)
    conan export recipes/manifold --version=3.2.1
    conan install . --build=missing "${conan_args[@]}"
    ;;
  configure)
    cmake --preset "$preset" \
      -DNODEHAMMER_WERROR=ON \
      -DNODEHAMMER_BUILD_TESTS=ON \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      "$@"
    ;;
  build)
    cmake --build --preset "$preset"
    ;;
  test)
    ctest --preset "$preset" --output-on-failure "$@"
    ;;
  install)
    prefix=${1:?install prefix required}
    cmake --install "$build_dir" --prefix "$prefix" "${install_extra[@]}"
    ;;
  *)
    echo "unknown command: $cmd" >&2
    exit 1
    ;;
esac
