#!/usr/bin/env bash
# Exercise an independent source build and its installed package. Extra CMake
# arguments can point FetchContent at cached sources; no Conan profile is needed.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
build=${1:?usage: verify-ingest.sh BUILD_DIR [CMake arguments...]}
shift
mkdir -p "$build"
build=$(cd "$build" && pwd)

cmake -S "$repo" -B "$build/library" -G Ninja \
    -DNODEHAMMER_INGEST_ONLY=ON -DNODEHAMMER_BUILD_TESTS=ON \
    -DNODEHAMMER_WERROR=ON -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$build/library" --parallel 4
ctest --test-dir "$build/library" --output-on-failure
cmake --install "$build/library" --prefix "$build/stage"
if [ "$(uname -s)" = "Linux" ]; then
    ingest_library=$(find "$build/stage" -name 'libnodehammer_ingest.so.*' -type f | head -1)
    "$repo/ci/check_shared_exports.py" "$ingest_library"
fi
cmake -DPREFIX="$build/stage" -DBUILD_DIR="$build/components" -DHAS_SHARED=OFF \
    -P "$repo/ci/verify-package-components.cmake"

# Backend choices must match the library. Read the cache rather than guess from
# the machine (which might have an unused ROOT installation).
backend_args=()
for backend in TGEO DD4HEP; do
    if grep -q "^NODEHAMMER_WITH_${backend}:BOOL=ON$" "$build/library/CMakeCache.txt"; then
        backend_args+=("-DCONSUMER_WITH_${backend}=ON")
    fi
done
cmake -S "$repo/ci/ingest_consumer" -B "$build/installed" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$build/stage" "${backend_args[@]}"
cmake --build "$build/installed" --parallel 4
PATH="$build/stage/bin:$PATH" ctest --test-dir "$build/installed" --output-on-failure

cmake -S "$repo/ci/ingest_consumer" -B "$build/embedded" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DNODEHAMMER_SOURCE_DIR="$repo" "${backend_args[@]}" "$@"
cmake --build "$build/embedded" --target consumer --parallel 4
ctest --test-dir "$build/embedded" --output-on-failure
