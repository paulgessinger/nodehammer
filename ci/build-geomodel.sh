#!/usr/bin/env bash
# Build only GeoModelCore and GeoModelIO with the CI environment's compiler.
# Keep this independent of Athena and of the detector database used locally.
set -euo pipefail
build=${1:?usage: build-geomodel.sh BUILD_DIR}
mkdir -p "$build"
build=$(cd "$build" && pwd)
# GeoModel 6.29.0, also the minimum supported by our importer.
revision=961bfb7d3af556d875d1b416796fb07266b2bb9d
export GIT_LFS_SKIP_SMUDGE=1
if [ ! -d "$build/source/.git" ]; then
    git init "$build/source"
fi
git -C "$build/source" fetch --depth 1 https://gitlab.cern.ch/GeoModelDev/GeoModel.git "$revision"
git -C "$build/source" checkout --detach FETCH_HEAD
cmake -S "$build/source" -B "$build/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$build/install" \
    -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=OFF \
    -DGEOMODEL_BUILD_ALL=OFF -DGEOMODEL_BUILD_TOOLS=OFF \
    -DGEOMODEL_BUILD_VISUALIZATION=OFF -DGEOMODEL_BUILD_TESTING=OFF
cmake --build "$build/build" --parallel 4
cmake --install "$build/build"
