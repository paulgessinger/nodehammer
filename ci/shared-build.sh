#!/usr/bin/env bash
# Local convenience wrapper: configure with NODEHAMMER_BUILD_SHARED=ON, build,
# install to a scratch prefix, and hand the result to
# ci/verify-shared-install.sh.
#
# CI does not use this — the build matrix already configures, builds and installs,
# so it runs the verify script against its own staged tree. This exists so the
# same check is one command away locally. Run after a `conan install`.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$here"

build_dir=${NH_SHARED_BUILD_DIR:-build/Release}
prefix=${NH_SHARED_PREFIX:-$PWD/build/shared-install}

echo "== configure (shared) =="
cmake --preset conan-release \
    -DNODEHAMMER_BUILD_SHARED=ON \
    -DNODEHAMMER_WERROR=ON

# Everything, not just --target nodehammer_shared: install() covers the CLI
# executable unconditionally, so a shared-only build leaves nothing to copy.
echo "== build =="
cmake --build "$build_dir"

echo "== install =="
rm -rf "$prefix"
cmake --install "$build_dir" --prefix "$prefix"

exec ci/verify-shared-install.sh "$prefix"
