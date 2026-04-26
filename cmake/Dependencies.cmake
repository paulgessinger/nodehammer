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

# ── ankerl::unordered_dense ───────────────────────────────────────────────────
# Open-addressed hash map — drop-in faster replacement for std::unordered_map
# on the hot scene lookups (scene.nodes etc.). Header-only.
FetchContent_Declare(unordered_dense
    SYSTEM
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v4.5.0
    FIND_PACKAGE_ARGS 4.0
)
FetchContent_MakeAvailable(unordered_dense)

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

# ── Viewer dependencies (bgfx + SDL3 + Dear ImGui) ────────────────────────────
# Only fetched when NODEHAMMER_WITH_VIEWER is ON. bgfx is wired via the
# bkaradzic/bgfx.cmake wrapper (which builds bgfx + bx + bimg + shaderc as plain
# CMake targets). The CCI Conan recipe for bgfx is currently broken on
# emscripten (PR #29596 open), so we always go through FetchContent for it.
# SDL3 and ImGui can come from Conan if available; FetchContent is the fallback.
if(NODEHAMMER_WITH_VIEWER)
    # bgfx via bgfx.cmake — multi-target wrapper
    FetchContent_Declare(bgfx
        SYSTEM
        GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git
        GIT_TAG        v1.136.9135-512
    )
    set(BGFX_BUILD_EXAMPLES        OFF CACHE BOOL "" FORCE)
    set(BGFX_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
    set(BGFX_INSTALL               OFF CACHE BOOL "" FORCE)
    set(BGFX_OPENGLES_VERSION      30  CACHE STRING "" FORCE)
    # bgfx.cmake itself disables BGFX_CONFIG_MULTITHREADED on emscripten via
    # cmake_dependent_option — do NOT override it here.
    #
    # Tools (shaderc, bin2c, ...) are host-only. We always disable them in this
    # subdirectory and rely on a host-built shaderc supplied via:
    #   1) Conan tool_requires (bgfx with tools=True, see conanfile.py) — adds
    #      the binaries to PATH via VirtualBuildEnv. This is the default path.
    #   2) An explicit shaderc on PATH (find_program in shaders/CMakeLists.txt).
    set(BGFX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(bgfx)

    # Locate a host-built shaderc and synthesise an IMPORTED bgfx::shaderc
    # target BEFORE we include bgfxToolUtils.cmake. The helper script gates
    # `bgfx_compile_shaders` behind `if(TARGET bgfx::shaderc)`, so the target
    # has to exist first or the function never gets defined.
    #
    # PATH source: typically Conan tool_requires (bgfx with tools=True) — see
    # conanfile.py's build_requirements(). Conan's CMakeToolchain prepends
    # build-context tool dirs to CMAKE_PROGRAM_PATH so find_program sees them.
    # Falls back to a system-installed shaderc if Conan didn't supply one.
    if(NOT TARGET bgfx::shaderc)
        find_program(NODEHAMMER_HOST_SHADERC shaderc
            DOC "Host-built bgfx shaderc (typically supplied via Conan tool_requires)")
        if(NODEHAMMER_HOST_SHADERC)
            add_executable(bgfx::shaderc IMPORTED GLOBAL)
            set_property(TARGET bgfx::shaderc PROPERTY
                IMPORTED_LOCATION "${NODEHAMMER_HOST_SHADERC}")
        endif()
    endif()

    # bgfx.cmake exposes bgfxToolUtils.cmake (bgfx_compile_shaders helper).
    # When consuming via add_subdirectory it isn't auto-included; pull it in.
    # Also set BGFX_SHADER_INCLUDE_PATH (which Config.cmake.in sets for installs
    # but isn't populated for add_subdirectory) so shaderc can find bgfx_shader.sh.
    if(EXISTS "${bgfx_SOURCE_DIR}/cmake/bgfxToolUtils.cmake")
        include("${bgfx_SOURCE_DIR}/cmake/bgfxToolUtils.cmake")
    endif()
    set(BGFX_SHADER_INCLUDE_PATH "${bgfx_SOURCE_DIR}/bgfx/src" CACHE INTERNAL
        "Path to bgfx_shader.sh and friends, used by bgfx_compile_shaders")

    # SDL3 — Conan provides SDL3::SDL3 / SDL3::SDL3-static; FetchContent build
    # uses the same target names since 3.2.
    FetchContent_Declare(SDL3
        SYSTEM
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.2.20
        FIND_PACKAGE_ARGS 3.2
    )
    set(SDL_SHARED  OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC  ON  CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS   OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)

    # Dear ImGui — no upstream CMake. Fetch sources, then create a static
    # target that compiles core + the SDL3 backend. The bgfx backend lives
    # in-tree under src/viewer/.
    FetchContent_Declare(imgui
        SYSTEM
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
    )
    FetchContent_MakeAvailable(imgui)

    if(NOT TARGET imgui)
        add_library(imgui STATIC
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        )
        target_include_directories(imgui SYSTEM PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )
        target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(imgui PRIVATE -w)
        elseif(MSVC)
            target_compile_options(imgui PRIVATE /W0)
        endif()
    endif()

    # Quiet third-party warnings on bgfx/SDL3 too — same pattern as zstd/manifold.
    foreach(_t bgfx bx bimg bimg_decode bimg_encode SDL3-static)
        if(TARGET ${_t})
            if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
                target_compile_options(${_t} PRIVATE -w)
            elseif(MSVC)
                target_compile_options(${_t} PRIVATE /W0)
            endif()
        endif()
    endforeach()

    # bgfx.cmake unconditionally defines bimg_encode (offline texture compression
    # — NVTT, astc_encoder, libsquish, ...). Only the host tool `texturec` links
    # it, and we have BGFX_BUILD_TOOLS=OFF, so nothing in our build needs it.
    # Excluding it from the default build sidesteps NVTT's emscripten-incompatible
    # C99 `restrict` usage without rewriting source.
    if(TARGET bimg_encode)
        set_target_properties(bimg_encode PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()
endif()
