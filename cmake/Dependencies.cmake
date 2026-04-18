include(FetchContent)

# ── zstd ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(zstd
    SYSTEM
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    SOURCE_SUBDIR  build/cmake
    FIND_PACKAGE_ARGS 1.5.6
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
    GIT_TAG        v3.7.1
    FIND_PACKAGE_ARGS 3.7.1
)
FetchContent_MakeAvailable(Catch2)

# ── CLI11 ─────────────────────────────────────────────────────────────────────
FetchContent_Declare(CLI11
    SYSTEM
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    FIND_PACKAGE_ARGS 2.4.2
)
FetchContent_MakeAvailable(CLI11)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    SYSTEM
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    FIND_PACKAGE_ARGS 3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ── GLM ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    SYSTEM
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    FIND_PACKAGE_ARGS 1.0.1
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
    GIT_TAG        v3.4.0
    FIND_PACKAGE_ARGS 3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# ── tinygltf ──────────────────────────────────────────────────────────────────
# Header-only glTF 2.0 reader/writer. Uses nlohmann_json (made available above).
FetchContent_Declare(tinygltf
    SYSTEM
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.7
    FIND_PACKAGE_ARGS 2.9.7
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
        GIT_TAG        v3.2.1
        FIND_PACKAGE_ARGS 3.2
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

# ── FlatBuffers ──────────────────────────────────────────────────────────────
FetchContent_Declare(flatbuffers
    SYSTEM
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.12.19
    FIND_PACKAGE_ARGS 25.12.19
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
