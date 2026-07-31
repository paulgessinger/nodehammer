#!/usr/bin/env bash
# The shared-build check — #41 step 5's verification.
#
# Configures with NODEHAMMER_BUILD_SHARED=ON, installs, and then asks the two
# questions that no in-tree build can answer:
#
#   1. Did anything escape into the export table? (ci/check_shared_exports.py)
#   2. Can somebody outside the build tree actually use the result — with no
#      dependency hints, no include paths, and no source access?
#      (ci/shared_consumer/)
#
# Run locally with `ci/shared-build.sh` after a `conan install`.
#
# Step 1 is ELF-only and skipped elsewhere, for opposite reasons on the two
# other platforms. macOS: ld64 has no --exclude-libs (only per-archive
# -hidden-l), so the dependencies' symbols do stay visible in the dylib and the
# check would fail for something that is not a defect. Windows: there is nothing
# to check, because PE/COFF exports nothing without __declspec(dllexport) — the
# property this script asserts on ELF is the platform default there, which is
# also why NH_API has a distinct static spelling (include/nodehammer/api.hpp).
#
# Step 2 runs everywhere, and on Windows it is the whole of the verification.

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

# Everything, not just --target nodehammer_shared. install() covers the CLI
# executable unconditionally, so a shared-only build leaves cmake --install with
# nothing to copy — which a developer's already-populated build tree hides and a
# clean CI checkout does not. Building the lot also makes this job verify that
# turning the option on does not disturb the ordinary targets, which is worth
# more than the minutes it costs.
echo "== build =="
cmake --build "$build_dir"

echo "== install =="
rm -rf "$prefix"
cmake --install "$build_dir" --prefix "$prefix"

# Nothing but the public headers may be installed. Cheap to assert directly,
# and it fails with a far clearer message than a consumer's include error.
echo "== installed headers =="
installed_headers=$(cd "$prefix/include" && find . -name '*.hpp' | sort)
echo "$installed_headers"
# Empty is its own failure and needs its own message: `echo ""` emits one blank
# line, which the pattern below does not match, so an empty install would
# otherwise be reported as headers being in the wrong place.
if [ -z "$installed_headers" ]; then
    echo "error: no headers installed under $prefix/include" >&2
    exit 1
fi
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
    echo "skipped: ELF only (see the header of this script)"
fi

echo "== consumer =="
rm -rf "$consumer_build"
# CMAKE_PREFIX_PATH deliberately carries *only* nodehammer's install prefix: if
# the package config needed a find_dependency() for zstd/flatbuffers/manifold,
# this is where it would fail.
cmake -S ci/shared_consumer -B "$consumer_build" -G Ninja \
    -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$consumer_build"

# Windows puts the DLL in the runtime dir (bin/) rather than beside the import
# library, and has no rpath to record where it went — so the loader needs PATH.
# Harmless elsewhere: on ELF/Mach-O the consumer already carries an rpath.
consumer_exe="$consumer_build/consumer"
[ -x "$consumer_exe" ] || consumer_exe="$consumer_build/consumer.exe"
PATH="$prefix/bin:$PATH" "$consumer_exe"

echo "== ok =="
