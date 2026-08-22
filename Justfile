set windows-shell := ["cmd.exe", "/C"]

# Vendored Conan recipes:
#   - sokol-shdc: wraps the prebuilt shader compiler binary from
#     floooh/sokol-tools-bin (used as a tool_requires when viewer=True).

ccache_launcher_arg := if os_family() == "windows" { "" } else { "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache" }
configure_native_cmd := if os_family() == "windows" { "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/windows-msvc-cmake.ps1 configure" } else { "cmake --preset conan-relwithdebinfo --fresh -GNinja " + ccache_launcher_arg + " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DNODEHAMMER_BUILD_TESTS=ON -DNODEHAMMER_WITH_VIEWER=ON" }
configure_native_incremental_cmd := if os_family() == "windows" { "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/windows-msvc-cmake.ps1 configure-incremental" } else { "cmake --preset conan-relwithdebinfo -GNinja " + ccache_launcher_arg + " -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DNODEHAMMER_BUILD_TESTS=ON -DNODEHAMMER_WITH_VIEWER=ON" }
build_native_cmd := if os_family() == "windows" { "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/windows-msvc-cmake.ps1 build" } else { "cmake --build --preset conan-relwithdebinfo" }
build_nodehammer_cmd := if os_family() == "windows" { "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/windows-msvc-cmake.ps1 build-nodehammer" } else { "cmake --build --preset conan-relwithdebinfo --target nodehammer" }

# Cheap to re-export every time; conan dedupes by recipe revision.
recipes:
    conan export recipes/cli11 --version=2.7.2
    conan export recipes/sokol --version=2026.07.02
    conan export recipes/imgui --version=1.92.8
    conan export recipes/implot --version=1.0.0
    conan export recipes/nfd --version=1.3.0
    conan export recipes/sokol-shdc --version=2026.06.13

deps: recipes
    conan install . -s build_type=RelWithDebInfo --build=missing -c tools.cmake.cmaketoolchain:generator=Ninja -o viewer=True

configure:
    {{configure_native_cmd}}

configure-incremental:
    {{configure_native_incremental_cmd}}

build:
    {{build_native_cmd}}

build-nodehammer:
    {{build_nodehammer_cmd}}

test:
    ctest --preset conan-relwithdebinfo --output-on-failure -j14

lint:
    prek run --all-files

# ── Python bindings ───────────────────────────────────────────────────────────
# Opt-in rather than folded into `just configure`: enabling the extension needs
# an interpreter with nanobind, and a plain `just configure` must not start
# failing for anyone who has not asked for the bindings.

# Create the virtualenv the bindings are built against and tested with
python-deps:
    uv venv --python 3.12 .venv
    uv pip install --python .venv --group dev

# Configure, build and test the Python bindings (run `just python-deps` first)
python: python-configure python-build python-test

python-configure:
    cmake --preset conan-relwithdebinfo -GNinja {{ccache_launcher_arg}} \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DNODEHAMMER_BUILD_TESTS=ON \
        -DNODEHAMMER_BUILD_PYTHON=ON \
        -DPython_EXECUTABLE={{justfile_directory()}}/.venv/bin/python

python-build:
    cmake --build --preset conan-relwithdebinfo --target nodehammer_python_package

# The same test CI runs, as ctest test #511
python-test:
    ctest --preset conan-relwithdebinfo -R python_bindings --output-on-failure

# The same suite directly, when you want per-case output rather than one verdict
pytest *args:
    PYTHONPATH={{justfile_directory()}}/build/RelWithDebInfo/python \
        {{justfile_directory()}}/.venv/bin/python -m pytest tests/python {{args}}

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
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
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

wasm-deps: _require-emsdk recipes
    conan install . \
        -pr:h profiles/emscripten \
        -pr:b default \
        -s build_type=RelWithDebInfo \
        -c tools.cmake.cmake_layout:build_folder_vars='["settings.os"]' \
        --build=missing \
        -o viewer=True

wasm-configure *args: _require-emsdk
    cmake --preset conan-emscripten-relwithdebinfo --fresh \
        -GNinja \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DNODEHAMMER_WITH_VIEWER=ON \
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

# Release wasm build: -Oz + LTO + closure + assertions stripped. Slower link,
# smaller .wasm/.js, harder-to-read browser stack traces. Use for shipping.
wasm-deps-release: _require-emsdk recipes
    conan install . \
        -pr:h profiles/emscripten \
        -pr:b default \
        -s build_type=Release \
        -c tools.cmake.cmake_layout:build_folder_vars='["settings.os"]' \
        --build=missing \
        -o viewer=True

wasm-configure-release *args: _require-emsdk
    cmake --preset conan-emscripten-release --fresh \
        -GNinja \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DNODEHAMMER_WITH_VIEWER=ON \
        -DNODEHAMMER_BUILD_TESTS=ON {{args}}

wasm-build-release: _require-emsdk
    cmake --build --preset conan-emscripten-release

wasm-release: wasm-deps-release wasm-configure-release wasm-build-release

# Serve the wasm viewer locally. Browsers refuse to load .wasm from file://,
# so we need a real http server. The static viewer.html shell probes
# navigator.gpu and dynamically loads either nodehammer-gles3.{js,wasm}
# (WebGL2) or nodehammer-wgpu.{js,wasm} (WebGPU).
#
# The shell picks its posture by fetching a sibling `nh_manifest.json` at
# startup:
#   - If present, its `archive` field names the `.nhproj` to load — viewer
#     mode, content-locked.
#   - If 404, the viewer comes up as the application: empty, restoring the
#     last project from IndexedDB, accepting drag-and-drop / file-picker.
#
# `scene` names any fixtures/configs/<scene>.toml; its full include chain is
# packed into the archive automatically. Pass `none` for application mode.
# `scene` can also be a path to an already-built .nhproj (any provenance --
# e.g. a Lua-config archive made natively via File > Create archive from
# scene > Save As, since make_nhproj.py only packs TOML scenes); it is staged
# as-is instead of packed from fixtures/configs.
#
# Usage:
#   just wasm-serve                                    # ODD scene, viewer mode (default)
#   just wasm-serve odd_simple                         # simplified ODD scene, viewer mode
#   just wasm-serve odd_drop_coincident_faces           # ODD with calorimeter/long-strip interior faces dropped
#   just wasm-serve none                                # no sidecar; application mode
#   just wasm-serve odd 9000                            # ODD scene on a custom port
#   just wasm-serve /tmp/my-lua-scene.nhproj            # pre-built archive, viewer mode
wasm-serve scene='odd' port='8000':
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    dir="$root/build/emscripten/RelWithDebInfo"
    mapfile -t staged < <("$root/scripts/stage_wasm_viewer.sh" link "{{scene}}" "$dir")
    mode="${staged[1]}"

    echo "serving $dir in $mode at:"
    echo "  http://localhost:{{port}}/viewer.html"
    # serve_nocache.py sends Cache-Control: no-store on every response. The
    # compute worker fetches its .wasm from inside a Web Worker, and browsers
    # don't honor hard-reload / DevTools "Disable Cache" for worker-initiated
    # requests -- so a plain http.server silently serves a stale worker module
    # after a rebuild. no-store sidesteps that; a rebuild is always picked up.
    cd "$dir" && exec python3 "$root/scripts/serve_nocache.py" {{port}}

# Copy the same static viewer payload that `wasm-serve` exposes into a
# self-contained directory. Sources from the Release wasm build by default;
# pass `build=RelWithDebInfo` to deploy the unminified (no-closure, -O2) build
# instead — useful for A/B-ing a Release-only browser bug against a known-good
# build. The target may be relative to the repo root or absolute. Build the
# chosen config first (`just wasm-release` for Release, `just wasm` for
# RelWithDebInfo).
#
# `scene` names any fixtures/configs/<scene>.toml; its full include chain is
# packed into the archive automatically. Pass `none` for application mode.
#
# Usage:
#   just wasm-copy                                              # ODD viewer-mode bundle (Release) to build/wasm-viewer
#   just wasm-copy odd public/viewer                            # ODD viewer-mode bundle (Release) to a custom directory
#   just wasm-copy odd_simple public/viewer                     # simplified ODD viewer-mode bundle (Release)
#   just wasm-copy odd_drop_coincident_faces public/viewer      # ODD with calorimeter/long-strip interior faces dropped
#   just wasm-copy none public/viewer                           # application-mode bundle (Release)
#   just wasm-copy odd public/viewer RelWithDebInfo             # deploy the RelWithDebInfo build instead
wasm-copy scene='odd' target='build/wasm-viewer' build='Release':
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    src="$root/build/emscripten/{{build}}"
    mapfile -t staged < <("$root/scripts/stage_wasm_viewer.sh" copy "{{scene}}" "{{target}}" "$src")
    out="${staged[0]}"
    mode="${staged[1]}"

    echo "copied wasm viewer payload to $out in $mode"

# Assemble the full GitHub Pages site locally — the landing page plus all three
# posture directories (application, ODD full, ODD simplified). Same script CI
# publishes with, so a local run previews exactly what gets deployed. Build the
# wasm first (`just wasm-release`), then serve the result:
#
#   just pages-site && python3 scripts/serve_nocache.py 8000 --directory build/pages
pages-site target='build/pages' build='Release':
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    "$root/scripts/build_pages_site.sh" "$root/build/emscripten/{{build}}" "{{target}}"

# Headless smoke for the compute-worker module: drives nh_compute_build in node
# via the wasm compute module on the ODD fixture, asserting NHR8 render bytes
# come back. Only needs `just wasm-build` -- nh_compute_build takes raw semantic
# bytes, so the .zst is expanded here rather than by the module.
#
# Pass `Release` to run against the closure'd module -- that is the only
# configuration where Closure renaming can break the JS<->wasm boundary, so it
# is worth a pass after `just wasm-build-release`:
#     just wasm-compute-smoke Release
wasm-compute-smoke build='RelWithDebInfo':
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    module="$root/build/emscripten/{{build}}/nodehammer-compute.js"
    fixture="$(mktemp -t nh_compute_smoke.XXXXXX.nhb)"
    trap 'rm -f "$fixture"' EXIT
    zstd -dcf "$root/odd.nhb.zst" > "$fixture"
    node "$root/scripts/wasm_compute_smoke.js" "$module" "$fixture"

# ── Python wheel ──────────────────────────────────────────────────────────────
# One deployment target, reaching both halves of the build.
#
# 13.3 is not a preference, it is the floor this code compiles to: Apple's libc++
# gates floating-point std::to_chars behind macOS 13.3, and <format> uses it, so
# anything lower fails to compile with "'to_chars' is unavailable". The arm64
# minimum of 11.0 is therefore unreachable while the tree uses std::format.
#
# It has to reach Conan as well as CMake. Set it only in the environment and the
# dependencies still get built at the host's default, so the linker warns
# ("object file was built for newer 'macOS' version") and the wheel claims a
# floor its own payload does not honour -- which delocate checks and rejects, and
# which would otherwise crash on a machine old enough to care.
# Read with sed rather than a TOML parser because `just` evaluates backtick
# assignments eagerly: this runs on every `just` invocation, on every platform,
# including the ones that never look at the value.
macos_deployment_target := `sed -n 's/^MACOSX_DEPLOYMENT_TARGET = "\(.*\)"/\1/p' pyproject.toml`

# Resolve dependencies for a wheel build (Release, no viewer, pinned floor)
wheel-deps: recipes
    conan install . \
        -s build_type=Release \
        -s:a compiler.cppstd=23 \
        {{ if os() == "macos" { "-s os.version=" + macos_deployment_target } else { "" } }} \
        -c tools.cmake.cmaketoolchain:generator=Ninja \
        --build=missing

# Conan runs first (see wheel-deps): its toolchain carries cppstd, libcxx and the
# deployment target, none of which a CMake dependency provider can deliver -- so
# the path is passed in rather than discovered.

# Build the Python wheel (run `just wheel-deps` first)
wheel *args:
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    tc="$root/build/Release/generators/conan_toolchain.cmake"
    if [ ! -f "$tc" ]; then
        echo "run 'just wheel-deps' first ($tc is missing)" >&2
        exit 1
    fi
    export CMAKE_TOOLCHAIN_FILE="$tc"
    if [ "$(uname)" = "Darwin" ]; then
        export MACOSX_DEPLOYMENT_TARGET="{{macos_deployment_target}}"
    fi
    uv build --wheel --out-dir "$root/dist" {{args}}
    # nanobind_add_module(STABLE_ABI) degrades *silently* to a version-specific
    # module when find_package(Python) did not turn up Development.SABIModule, and
    # the only visible difference is the filename. Catch that here rather than
    # discovering it as a CI matrix that grew from one wheel per platform to one
    # per interpreter.
    # dist/ also holds nodehammer_web-*.whl, which is py3-none-any by design, so
    # the glob names this distribution rather than every wheel in the directory.
    for whl in "$root"/dist/nodehammer-*.whl; do
        case "$whl" in
            *-abi3-*) ;;
            *) echo "not an abi3 wheel: $(basename "$whl") -- STABLE_ABI did not engage" >&2
               exit 1 ;;
        esac
    done

# ── the web runtime wheel ─────────────────────────────────────────────────────
# `nodehammer-web` is the other half of the Python story: a py3-none-any wheel
# holding the Emscripten build, because no native toolchain can produce one and
# a headless install has no use for 7 MB of it. See packaging/web/pyproject.toml.
#
# `src` is anything the runtime locator would accept -- a wasm build directory or
# an install tree's share/nodehammer/web -- and it is staged by the same script
# the Pages site uses, in application mode, so the payload is the one CI deploys.

# Build the nodehammer-web wheel from a wasm build (run `just wasm-release` first)
wheel-web src="build/emscripten/Release":
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    src="{{src}}"
    case "$src" in /*) ;; *) src="$root/$src" ;; esac
    runtime="$root/packaging/web/src/nodehammer_web/runtime"
    # Both sides emptied rather than overlaid. The staging directory is the
    # obvious one; setuptools' build/lib is the one that bites, because
    # `build_py` copies into it and never prunes -- a file dropped from the
    # payload survives there and is added to the wheel from a previous run.
    # Caught doing exactly that with a stale nodehammer-wgpu.wasm.zst.
    rm -rf "$runtime" "$root/packaging/web/build"
    "$root/scripts/stage_wasm_viewer.sh" copy none "$runtime" "$src"
    uv build --wheel --out-dir "$root/dist" "$root/packaging/web"
    # The mirror of the abi3 check above, and the same kind of silent failure:
    # a wheel that picked up a platform tag would install nowhere it is needed.
    for whl in "$root"/dist/nodehammer_web-*.whl; do
        case "$whl" in
            *-py3-none-any.whl) ;;
            *) echo "not a pure wheel: $(basename "$whl")" >&2
               exit 1 ;;
        esac
    done

# The only check that reaches the rung a wheel fills. `--web-assets` and
# NODEHAMMER_WEB_ASSETS are cleared out of the way on purpose: both outrank the
# package, so leaving either set would test the wrong rung and pass anyway.
#
# The pair check reads dist/, so it fails on a directory holding two versions of
# either distribution -- which is the stale-artifact case it exists to catch, and
# locally usually just means `rm -rf dist` first.

# Install both wheels from local files and serve the viewer out of them
wheel-web-test: wheel wheel-web
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    uv run --no-project --with packaging "$root/scripts/check_wheel_pair.py" "$root/dist"
    venv=$(mktemp -d)/venv
    uv venv --python 3.12 "$venv"
    # --no-index: the pair under test is the one just built, never whatever an
    # index happens to resolve. Only `nodehammer` is named -- pulling the runtime
    # in is the dependency's job, and naming it here would pass even if the
    # generated pin had gone missing.
    VIRTUAL_ENV="$venv" uv pip install --quiet --no-index \
        --find-links "$root/dist" nodehammer
    env -u NODEHAMMER_WEB_ASSETS "$root/scripts/check_web_serve.py" "$venv/bin/nodehammer"

# The Python counterpart of ci/shared_consumer: the build tree resolves
# libnodehammer through CMake's own rpath, so only an installed wheel proves the
# relative rpath and the bundled library are right.

# Install the built wheel into a throwaway venv and run the suite against it
wheel-test: wheel
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    venv=$(mktemp -d)/venv
    uv venv --python 3.12 "$venv"
    VIRTUAL_ENV="$venv" uv pip install --quiet pytest tomli-w
    # --no-deps, in its own command so the test tools above still resolve.
    # `nodehammer` requires `nodehammer-web`, which is built by a separate
    # Emscripten toolchain -- resolving it here would either reach an index for
    # something this recipe is not testing, or fail on a machine with no emsdk.
    # What this recipe is for is the platform wheel's own payload and its
    # relative rpath; `just wheel-web-test` is the one that exercises the pair.
    VIRTUAL_ENV="$venv" uv pip install --quiet --no-deps "$root"/dist/nodehammer-*.whl
    cd "$root" && "$venv/bin/python" -m pytest tests/python -q

# Build the Linux wheels the way CI does, in a manylinux container. Needs Docker.
# The same cibuildwheel configuration CI uses, so a failure here is a failure
# there -- including `auditwheel repair`, which is the check that decides whether
# the library's libstdc++ references fit the manylinux policy.
wheel-linux *args:
    #!/usr/bin/env bash
    set -euo pipefail
    root="{{justfile_directory()}}"
    # The container has no .git, so setuptools_scm has to be told the answer from
    # out here. Same override the publish job uses, so the version a local Linux
    # wheel carries is the version CI would upload.
    export SETUPTOOLS_SCM_PRETEND_VERSION=$(
        SETUPTOOLS_SCM_OVERRIDES_FOR_NODEHAMMER='{local_scheme = "no-local-version"}' \
        uv run --no-project --with setuptools-scm python -m setuptools_scm | tail -1)
    echo "version: $SETUPTOOLS_SCM_PRETEND_VERSION"
    uvx cibuildwheel --platform linux --output-dir "$root/dist" {{args}}
