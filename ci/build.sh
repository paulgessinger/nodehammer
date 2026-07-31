#!/usr/bin/env bash
# Shared CI helper used by .github/workflows/ci.yml (build matrix + LCG job).
#
# Dispatcher style so each CI step keeps its own log block while the
# kind → conan/cmake-preset mapping lives in one place.
#
# Usage:
#   ci/build.sh conan-install <kind> [extra conan args...]
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
    # Every native CI build also builds and installs the shared library, so that
    # ci/verify-shared-install.sh can run against the tree the job already
    # staged. Stated here rather than per-leg in the workflow, which is what
    # keeps the build matrix free of a `shared:` field: the one place the option
    # does not apply is Emscripten, and that is already a different kind.
    # NODEHAMMER_BUILD_SHARED under Emscripten is a hard CMake error by design.
    configure_extra=(-DNODEHAMMER_BUILD_SHARED=ON)
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
    configure_extra=()
    ;;
  *)
    echo "unknown kind: $kind" >&2
    exit 1
    ;;
esac

case "$cmd" in
  conan-install)
    conan export recipes/sokol --version=2026.07.02
    conan export recipes/imgui --version=1.92.8
    conan export recipes/implot --version=1.0.0
    conan export recipes/nfd --version=1.3.0
    conan export recipes/sokol-shdc --version=2026.06.13
    conan install . --build=missing "${conan_args[@]}" "$@"
    ;;
  configure)
    cmake --preset "$preset" \
      -DNODEHAMMER_WERROR=ON \
      -DNODEHAMMER_BUILD_TESTS=ON \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      "${configure_extra[@]}" \
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
