include(FetchContent)

# ── zstd ──────────────────────────────────────────────────────────────────────
FetchContent_Declare(zstd
    SYSTEM
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.7
    SOURCE_SUBDIR  build/cmake
    FIND_PACKAGE_ARGS 1.5.7
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
    GIT_TAG        v3.15.1
    FIND_PACKAGE_ARGS 3.15.1
)
FetchContent_MakeAvailable(Catch2)

# ── CLI11 ─────────────────────────────────────────────────────────────────────
# 2.7.2 is a floor, not a preference. Up to and including 2.6.2 CLI11's character
# tables were namespace-scope references bound to temporaries, which Emscripten
# cannot run an atexit chain over -- see recipes/cli11/conanfile.py and issue #76.
FetchContent_Declare(CLI11
    SYSTEM
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.7.2
    FIND_PACKAGE_ARGS 2.7.2
)
FetchContent_MakeAvailable(CLI11)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    SYSTEM
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    FIND_PACKAGE_ARGS 3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

# ── GLM ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    SYSTEM
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    FIND_PACKAGE_ARGS 1.0.3
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
        GIT_TAG        v3.5.1
        FIND_PACKAGE_ARGS 3.5
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
    GIT_TAG        v4.8.1
    FIND_PACKAGE_ARGS 4.8.1
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

# ── Lua config front-end (lua + sol2) ────────────────────────────────────────
# Powers the `config-lua` CLI command and `Config::read`'s `.lua` branch — an
# unconditional source in nodehammer_lib on every platform, Emscripten included.
# Both come from Conan (lua has no clean upstream CMake — it ships a Makefile —
# so, like imgui/implot below, we resolve via find_package rather than a git
# FetchContent fallback).
# Targets: lua::lua, sol2::sol2 (sol2 links lua transitively).
find_package(lua REQUIRED CONFIG)
find_package(sol2 REQUIRED CONFIG)

# ── Viewer dependencies (sokol_gfx + Dear ImGui) ─────────────────────────────
# Only set up when NODEHAMMER_WITH_VIEWER is ON. sokol is single-header so we
# fetch the repo and let cmake/Sokol.cmake build per-backend STATIC libs from
# the small impl TUs in src/sokol/. No SDL3 — sokol_app owns the window/event
# loop on every platform, including Emscripten.
if(NODEHAMMER_WITH_VIEWER)
    # Dear ImGui. Pinned and packaged by the local recipe under recipes/imgui.
    # No SDL3 backend; input + rendering are wired through util/sokol_imgui.h.
    find_package(imgui REQUIRED CONFIG)

    # ── ImPlot ───────────────────────────────────────────────────────────────
    # Plotting widgets for Dear ImGui (live perf graphs in the Debug panel).
    # Packaged by the local recipe under recipes/implot against the pinned
    # Dear ImGui snapshot above.
    find_package(implot REQUIRED CONFIG)

    # Vendored Font Awesome 7 (free-solid). Header from juliettef/IconFontCppHeaders,
    # compressed font blob generated from the official FA7 desktop OTF via
    # imgui's misc/fonts/binary_to_compressed_c. Both files live under
    # third_party/fontawesome7/. INTERFACE-only target — consumers include
    # `<IconsFontAwesome7.h>` (macros) and `<fa-solid-900.h>` (compressed blob).
    if(NOT TARGET nh_fontawesome7)
        add_library(nh_fontawesome7 INTERFACE)
        target_include_directories(nh_fontawesome7 SYSTEM INTERFACE
            ${CMAKE_SOURCE_DIR}/third_party/fontawesome7
        )
    endif()

    # Vendored stb_image_write (single public-domain header). Conan's tinygltf
    # package ships only tiny_gltf.h, so the screenshot/PNG export carries its
    # own copy. INTERFACE-only target — consumers include <stb_image_write.h>;
    # exactly one viewer TU (stb_image_write_impl.cpp) defines the implementation.
    if(NOT TARGET nh_stb)
        add_library(nh_stb INTERFACE)
        target_include_directories(nh_stb SYSTEM INTERFACE
            ${CMAKE_SOURCE_DIR}/third_party/stb
        )
    endif()

    # sokol headers + sokol-shdc resolver + nh_add_sokol_lib + nh_compile_shader.
    include(${CMAKE_SOURCE_DIR}/cmake/Sokol.cmake)

    # ── nativefiledialog-extended ───────────────────────────────────────────
    # Native-only file picker (NFD::nfd target). The web build uses an
    # HTML <input type=file> + FileSystemAccess API instead; skip the dep
    # entirely under emscripten so the wasm binary doesn't grow a useless
    # AppKit/GTK/win32 stub. The local recipe defaults Linux to
    # xdg-desktop-portal instead of bundling GTK.
    # NODEHAMMER_VIEWER_NATIVE_DIALOG=OFF skips the dep entirely: the viewer
    # compiles a no-op picker (drag-and-drop and CLI --input still work) so it
    # can build on headless hosts / minimal containers without GTK or DBus.
    if(NOT EMSCRIPTEN AND NODEHAMMER_VIEWER_NATIVE_DIALOG)
        find_package(nfd REQUIRED CONFIG)
    endif()
endif()
