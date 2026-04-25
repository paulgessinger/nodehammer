include(FetchContent)

# ── Third-party metadata ─────────────────────────────────────────────────────
# Single source of truth for the versions and licenses of every third-party
# component. Each entry declares:
#   NH_DEP_<NAME>_VERSION      — version used across FetchContent / Conan / find_package
#   NH_DEP_<NAME>_LICENSE      — SPDX identifier
#   NH_DEP_<NAME>_LICENSE_URL  — raw URL to the license text at the pinned version
#
# scripts/harvest_licenses.py greps this file for the URLs and writes each
# dep's text to third_party_licenses/<slug>/LICENSE (committed). CMakeLists.txt
# assembles THIRD-PARTY-NOTICES.txt at configure time from the subset of slugs
# whose NODEHAMMER_WITH_<SLUG_UPPER> gate (if any) evaluates truthy, and
# installs that artifact alongside the binary. Run `just licenses` after a
# version bump; CI fails the `prek` job on drift.

set(NH_THIRD_PARTY
    zstd
    catch2
    cli11
    nlohmann_json
    glm
    tomlplusplus
    tinygltf
    stb
    manifold
    clipper2
    unordered_dense
    flatbuffers
    # Backend deps — resolved via find_package from Spack / LCG / a system
    # install, gated by NODEHAMMER_WITH_TGEO / NODEHAMMER_WITH_DD4HEP. Versions
    # here are documentation — bump to match whatever the shipping environment
    # actually provides.
    root
    dd4hep
)

set(NH_DEP_ZSTD_VERSION            1.5.6)
set(NH_DEP_ZSTD_LICENSE            BSD-3-Clause)
set(NH_DEP_ZSTD_LICENSE_URL        "https://raw.githubusercontent.com/facebook/zstd/v${NH_DEP_ZSTD_VERSION}/LICENSE")

set(NH_DEP_CATCH2_VERSION          3.7.1)
set(NH_DEP_CATCH2_LICENSE          BSL-1.0)
set(NH_DEP_CATCH2_LICENSE_URL      "https://raw.githubusercontent.com/catchorg/Catch2/v${NH_DEP_CATCH2_VERSION}/LICENSE.txt")

set(NH_DEP_CLI11_VERSION           2.4.2)
set(NH_DEP_CLI11_LICENSE           BSD-3-Clause)
set(NH_DEP_CLI11_LICENSE_URL       "https://raw.githubusercontent.com/CLIUtils/CLI11/v${NH_DEP_CLI11_VERSION}/LICENSE")

set(NH_DEP_NLOHMANN_JSON_VERSION   3.11.3)
set(NH_DEP_NLOHMANN_JSON_LICENSE   MIT)
set(NH_DEP_NLOHMANN_JSON_LICENSE_URL "https://raw.githubusercontent.com/nlohmann/json/v${NH_DEP_NLOHMANN_JSON_VERSION}/LICENSE.MIT")

set(NH_DEP_GLM_VERSION             1.0.1)
set(NH_DEP_GLM_LICENSE             MIT)
set(NH_DEP_GLM_LICENSE_URL         "https://raw.githubusercontent.com/g-truc/glm/${NH_DEP_GLM_VERSION}/copying.txt")

set(NH_DEP_TOMLPLUSPLUS_VERSION    3.4.0)
set(NH_DEP_TOMLPLUSPLUS_LICENSE    MIT)
set(NH_DEP_TOMLPLUSPLUS_LICENSE_URL "https://raw.githubusercontent.com/marzer/tomlplusplus/v${NH_DEP_TOMLPLUSPLUS_VERSION}/LICENSE")

set(NH_DEP_TINYGLTF_VERSION        2.9.7)
set(NH_DEP_TINYGLTF_LICENSE        MIT)
set(NH_DEP_TINYGLTF_LICENSE_URL    "https://raw.githubusercontent.com/syoyo/tinygltf/v${NH_DEP_TINYGLTF_VERSION}/LICENSE")

# stb is a transitive (via tinygltf) — no direct FetchContent/find_package.
# Upstream has no versioned tags; pin to the commit that matches the ConanCenter
# cci.20240531 snapshot to keep the license text stable.
set(NH_DEP_STB_VERSION             cci.20240531)
set(NH_DEP_STB_LICENSE             "MIT OR Unlicense")
set(NH_DEP_STB_LICENSE_URL         "https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/LICENSE")

set(NH_DEP_MANIFOLD_VERSION        3.2.1)
set(NH_DEP_MANIFOLD_LICENSE        Apache-2.0)
set(NH_DEP_MANIFOLD_LICENSE_URL    "https://raw.githubusercontent.com/elalish/manifold/v${NH_DEP_MANIFOLD_VERSION}/LICENSE")

# clipper2 is a transitive (conan transitive of manifold on native; builtin fetch
# inside manifold on wasm). Version matches what manifold 3.2.1 pulls.
set(NH_DEP_CLIPPER2_VERSION        1.4.0)
set(NH_DEP_CLIPPER2_LICENSE        BSL-1.0)
set(NH_DEP_CLIPPER2_LICENSE_URL    "https://raw.githubusercontent.com/AngusJohnson/Clipper2/Clipper2_${NH_DEP_CLIPPER2_VERSION}/LICENSE")

set(NH_DEP_UNORDERED_DENSE_VERSION 4.5.0)
set(NH_DEP_UNORDERED_DENSE_LICENSE MIT)
set(NH_DEP_UNORDERED_DENSE_LICENSE_URL "https://raw.githubusercontent.com/martinus/unordered_dense/v${NH_DEP_UNORDERED_DENSE_VERSION}/LICENSE")

set(NH_DEP_FLATBUFFERS_VERSION     25.9.23)
set(NH_DEP_FLATBUFFERS_LICENSE     Apache-2.0)
set(NH_DEP_FLATBUFFERS_LICENSE_URL "https://raw.githubusercontent.com/google/flatbuffers/v${NH_DEP_FLATBUFFERS_VERSION}/LICENSE")

# Backend deps are resolved from Spack/LCG/system; the version actually linked
# varies by deployment, so only the license SPDX id and a stable license-text
# URL are pinned here.
set(NH_DEP_ROOT_LICENSE            LGPL-2.1-or-later)
set(NH_DEP_ROOT_LICENSE_URL        "https://raw.githubusercontent.com/root-project/root/v6-34-08/LICENSE")

set(NH_DEP_DD4HEP_LICENSE          LGPL-3.0-or-later)
set(NH_DEP_DD4HEP_LICENSE_URL      "https://raw.githubusercontent.com/AIDASoft/DD4hep/v01-32/LICENSE")

# ── zstd ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(zstd
    SYSTEM
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v${NH_DEP_ZSTD_VERSION}
    SOURCE_SUBDIR  build/cmake
    FIND_PACKAGE_ARGS ${NH_DEP_ZSTD_VERSION}
)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zstd)

# FetchContent creates libzstd_static; normalize to the namespaced target
# that find_package provides.
if(TARGET libzstd_static AND NOT TARGET zstd::libzstd_static)
    add_library(zstd::libzstd_static ALIAS libzstd_static)
endif()

# Suppress warnings in third-party zstd build
if(TARGET libzstd_static)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(libzstd_static PRIVATE -w)
    elseif(MSVC)
        target_compile_options(libzstd_static PRIVATE /W0)
    endif()
endif()

# ── Catch2 v3 ─────────────────────────────────────────────────────────────────
FetchContent_Declare(Catch2
    SYSTEM
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v${NH_DEP_CATCH2_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_CATCH2_VERSION}
)
FetchContent_MakeAvailable(Catch2)

# ── CLI11 ─────────────────────────────────────────────────────────────────────
FetchContent_Declare(CLI11
    SYSTEM
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v${NH_DEP_CLI11_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_CLI11_VERSION}
)
FetchContent_MakeAvailable(CLI11)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    SYSTEM
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v${NH_DEP_NLOHMANN_JSON_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_NLOHMANN_JSON_VERSION}
)
FetchContent_MakeAvailable(nlohmann_json)

# ── GLM ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    SYSTEM
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        ${NH_DEP_GLM_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_GLM_VERSION}
)
# GLM 1.x uses the global BUILD_SHARED_LIBS to decide shared vs static.
# This is set before MakeAvailable; if other deps later need the opposite,
# save/restore the value around this block.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glm)

# ── toml++ ────────────────────────────────────────────────────────────────────
FetchContent_Declare(tomlplusplus
    SYSTEM
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v${NH_DEP_TOMLPLUSPLUS_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_TOMLPLUSPLUS_VERSION}
)
FetchContent_MakeAvailable(tomlplusplus)

# ── tinygltf ──────────────────────────────────────────────────────────────────
# Header-only glTF 2.0 reader/writer. Uses nlohmann_json (made available above).
FetchContent_Declare(tinygltf
    SYSTEM
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v${NH_DEP_TINYGLTF_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_TINYGLTF_VERSION}
)

# Disable tinygltf's own examples/tests
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL               OFF CACHE BOOL "" FORCE)
set(TINYGLTF_HEADER_ONLY           ON  CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(tinygltf)

# Canonical target is TinyGLTF::TinyGLTF (provided by Conan).
# FetchContent only creates plain tinygltf; alias to the namespaced name.
if(TARGET tinygltf AND NOT TARGET TinyGLTF::TinyGLTF)
    add_library(TinyGLTF::TinyGLTF ALIAS tinygltf)
endif()

# ── Manifold (boolean mesh operations) ─────────────────────────────────────────
FetchContent_Declare(manifold
        SYSTEM
        GIT_REPOSITORY https://github.com/elalish/manifold.git
        GIT_TAG        v${NH_DEP_MANIFOLD_VERSION}
        FIND_PACKAGE_ARGS ${NH_DEP_MANIFOLD_VERSION}
    )
    set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_EXPORT OFF CACHE BOOL "" FORCE)
    # Single-threaded under cross-compile (emscripten has no system TBB/OpenMP).
    # MANIFOLD_PAR is a plain bool — any non-empty value is truthy, so we must
    # set it to OFF explicitly (not "NONE"). Belt-and-suspenders: also disable
    # the builtin TBB fetch so manifold can't pull it in another way.
    # Also use manifold's bundled clipper2 fetch: the CCI manifold recipe
    # applies a target-name rename (clipper2::clipper2 -> Clipper2) that
    # manifold's own CMake depends on, and we can't replicate that via
    # FetchContent. Letting manifold own its clipper2 sidesteps the mismatch.
    if(CMAKE_CROSSCOMPILING OR EMSCRIPTEN)
        set(MANIFOLD_PAR OFF CACHE BOOL "" FORCE)
        set(MANIFOLD_USE_BUILTIN_TBB OFF CACHE BOOL "" FORCE)
        set(MANIFOLD_USE_BUILTIN_CLIPPER2 ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_MakeAvailable(manifold)

    # Global CMAKE_CXX_FLAGS / toolchain warnings apply to subprojects too; keep third-party
    # builds quiet so nodehammer's warning level does not flood Manifold/Clipper2.
    if(TARGET manifold)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(manifold PRIVATE -w)
        elseif(MSVC)
            target_compile_options(manifold PRIVATE /W0)
        endif()
    endif()
    if(TARGET Clipper2)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(Clipper2 PRIVATE -w)
        elseif(MSVC)
            target_compile_options(Clipper2 PRIVATE /W0)
        endif()
    endif()

# ── ankerl::unordered_dense ───────────────────────────────────────────────────
# Open-addressed hash map — drop-in faster replacement for std::unordered_map
# on the hot scene lookups (scene.nodes etc.). Header-only.
FetchContent_Declare(unordered_dense
    SYSTEM
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v${NH_DEP_UNORDERED_DENSE_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_UNORDERED_DENSE_VERSION}
)
FetchContent_MakeAvailable(unordered_dense)

# ── FlatBuffers ──────────────────────────────────────────────────────────────
FetchContent_Declare(flatbuffers
    SYSTEM
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v${NH_DEP_FLATBUFFERS_VERSION}
    FIND_PACKAGE_ARGS ${NH_DEP_FLATBUFFERS_VERSION}
)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# flatc is a host tool (runs at build time, not on the target). When cross-
# compiling (e.g. emscripten) we cannot build it here — expect one on PATH
# and resolve via find_program() in the main CMakeLists.
if(CMAKE_CROSSCOMPILING)
    set(FLATBUFFERS_BUILD_FLATC OFF CACHE BOOL "" FORCE)
else()
    set(FLATBUFFERS_BUILD_FLATC ON  CACHE BOOL "" FORCE)
endif()
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flatbuffers)

# Canonical target is flatbuffers::flatbuffers (provided by Conan).
# FetchContent only creates plain flatbuffers; alias to the namespaced name.
if(TARGET flatbuffers AND NOT TARGET flatbuffers::flatbuffers)
    add_library(flatbuffers::flatbuffers ALIAS flatbuffers)
endif()

# Silence warnings from the FetchContent-built flatbuffers (the Conan binary
# was built elsewhere, so its flags are already fixed).
if(TARGET flatbuffers)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(flatbuffers PRIVATE -w)
    elseif(MSVC)
        target_compile_options(flatbuffers PRIVATE /W0)
    endif()
endif()
