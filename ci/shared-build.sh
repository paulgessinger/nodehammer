#!/usr/bin/env bash
# Local convenience wrapper: configure with NODEHAMMER_BUILD_SHARED=ON, build,
# install to a scratch prefix, and hand the result to
# ci/verify-shared-install.sh.
#
# CI does not use this. The build matrix already configures, builds and installs
# every native leg, so it runs the verify script directly against its own staged
# install rather than doing all of that a second time. This script exists so the
# same check is one command away on a developer's machine.
#
# Run with `ci/shared-build.sh` after a `conan install`.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$here"

build_dir=${NH_SHARED_BUILD_DIR:-build/Release}
prefix=${NH_SHARED_PREFIX:-$PWD/build/shared-install}

echo "== configure (shared) =="
cmake --preset conan-release \
    -DNODEHAMMER_BUILD_SHARED=ON \
    -DNODEHAMMER_WERROR=ON

# Everything, not just --target nodehammer_shared. install() covers the CLI
# executable unconditionally, so a shared-only build leaves cmake --install with
# nothing to copy — which a developer's already-populated build tree hides and a
# clean checkout does not.
echo "== build =="
cmake --build "$build_dir"

echo "== install =="
rm -rf "$prefix"
cmake --install "$build_dir" --prefix "$prefix"

exec ci/verify-shared-install.sh "$prefix"
