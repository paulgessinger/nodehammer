#!/usr/bin/env bash
# The shared-build check — #41 step 5's verification.
#
# Configures with NODEHAMMER_BUILD_SHARED=ON, installs, and then asks the two
# questions that no in-tree build can answer:
#
#   1. Does the export table equal the public API? (ci/check_shared_exports.py)
#   2. Can somebody outside the build tree actually use the result — with no
#      dependency hints, no include paths, and no source access?
#      (ci/shared_consumer/)
#
# Run locally with `ci/shared-build.sh` after a `conan install`. On macOS step 1
# is skipped: ld64 has no --exclude-libs, so the static dependencies' symbols
# stay visible in the dylib and the check would fail for a reason that is not a
# defect. Step 2 runs everywhere.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$here"

build_dir=${NH_SHARED_BUILD_DIR:-build/Release}
prefix=${NH_SHARED_PREFIX:-$PWD/build/shared-install}
consumer_build=${NH_CONSUMER_BUILD_DIR:-build/shared-consumer}

# Runs everywhere, including where there is no ELF library to inspect, so the
# rules stay honest on macOS and on any run that fails before the link.
echo "== export-rule self-test =="
ci/check_shared_exports.py --self-test

echo "== configure (shared) =="
cmake --preset conan-release \
    -DNODEHAMMER_BUILD_SHARED=ON \
    -DNODEHAMMER_WERROR=ON \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

echo "== build the shared library =="
cmake --build "$build_dir" --target nodehammer_shared

echo "== install =="
rm -rf "$prefix"
cmake --install "$build_dir" --prefix "$prefix"

# Nothing but the public headers may be installed. Cheap to assert directly,
# and it fails with a far clearer message than a consumer's include error.
echo "== installed headers =="
installed_headers=$(cd "$prefix/include" && find . -name '*.hpp' | sort)
echo "$installed_headers"
if echo "$installed_headers" | grep -vqE '^\./nodehammer/[^/]+\.hpp$'; then
    echo "error: installed headers outside include/nodehammer/" >&2
    exit 1
fi

echo "== export table =="
if [ "$(uname -s)" = "Linux" ]; then
    lib=$(find "$prefix" -name 'libnodehammer.so*' -type f | head -1)
    if [ -z "$lib" ]; then
        echo "error: no libnodehammer.so under $prefix" >&2
        exit 1
    fi
    ci/check_shared_exports.py "$lib"
else
    echo "skipped: ELF only (ld64 has no --exclude-libs; see cmake/PicProbe.cmake)"
fi

echo "== consumer =="
rm -rf "$consumer_build"
# CMAKE_PREFIX_PATH deliberately carries *only* nodehammer's install prefix: if
# the package config needed a find_dependency() for zstd/flatbuffers/manifold,
# this is where it would fail.
cmake -S ci/shared_consumer -B "$consumer_build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$consumer_build"
"$consumer_build/consumer"

echo "== ok =="
