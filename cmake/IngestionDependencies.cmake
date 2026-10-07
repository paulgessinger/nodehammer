# The complete dependency set for geometry ingestion. Heavy experiment
# backends are resolved separately and are never fetched.
include_guard(GLOBAL)
include(FetchContent)

function(nh_ingestion_dependencies)
    include(GNUInstallDirs)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    if(NODEHAMMER_INGEST_ONLY)
        set(GLM_BUILD_LIBRARY OFF)
        set(GLM_BUILD_INSTALL OFF)
        set(FLATBUFFERS_INSTALL OFF)
        set(FLATBUFFERS_BUILD_FLATLIB OFF)
    endif()
    if(NOT EMSCRIPTEN)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    endif()
    # ── zstd ──────────────────────────────────────────────────────────────────────
    FetchContent_Declare(zstd
        SYSTEM
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG        v1.5.7
        SOURCE_SUBDIR  build/cmake
        FIND_PACKAGE_ARGS 1.5.7
    )
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_CONTRIB OFF)
    set(ZSTD_BUILD_STATIC ON)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    if(NOT TARGET zstd::libzstd_static)
        FetchContent_MakeAvailable(zstd)
        if(zstd_SOURCE_DIR)
            # zstd has no install-off option. Keep its subdirectory out of our
            # install; the required static target still builds through linking.
            set_property(DIRECTORY "${zstd_SOURCE_DIR}/build/cmake"
                PROPERTY EXCLUDE_FROM_ALL TRUE)
            install(FILES "${zstd_SOURCE_DIR}/LICENSE"
                DESTINATION ${CMAKE_INSTALL_DATADIR}/nodehammer/licenses/zstd
                COMPONENT Runtime)
        endif()
    endif()

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

    # ── GLM ───────────────────────────────────────────────────────────────────────
    FetchContent_Declare(glm
        SYSTEM
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG        1.0.3
        FIND_PACKAGE_ARGS 1.0.3
    )
    # GLM 1.x uses the global BUILD_SHARED_LIBS to decide shared vs static.
    # Keep the setting local to this dependency helper.
    set(BUILD_SHARED_LIBS OFF)
    if(NOT TARGET glm::glm)
        FetchContent_MakeAvailable(glm)
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
    if(NOT TARGET unordered_dense::unordered_dense)
        FetchContent_MakeAvailable(unordered_dense)
    endif()

    # ── FlatBuffers ──────────────────────────────────────────────────────────────
    FetchContent_Declare(flatbuffers
        SYSTEM
        GIT_REPOSITORY https://github.com/google/flatbuffers.git
        GIT_TAG        v25.12.19
        FIND_PACKAGE_ARGS 25.12.19
    )
    set(FLATBUFFERS_BUILD_TESTS OFF)
    # flatc is a host tool (runs at build time, not on the target). When cross-
    # compiling (e.g. emscripten) we cannot build it here — expect one on PATH
    # and resolve via find_program() in Ingestion.cmake.
    if(CMAKE_CROSSCOMPILING)
        set(FLATBUFFERS_BUILD_FLATC OFF)
    else()
        set(FLATBUFFERS_BUILD_FLATC ON)
    endif()
    set(FLATBUFFERS_BUILD_FLATHASH OFF)
    if(NOT TARGET flatbuffers::flatbuffers)
        FetchContent_MakeAvailable(flatbuffers)
    endif()

    # Canonical target is flatbuffers::flatbuffers (provided by Conan).
    # FetchContent creates flatbuffers or the header-only FlatBuffers target.
    if(NOT TARGET flatbuffers::flatbuffers)
        if(TARGET flatbuffers)
            add_library(flatbuffers::flatbuffers ALIAS flatbuffers)
        elseif(TARGET FlatBuffers)
            add_library(flatbuffers::flatbuffers ALIAS FlatBuffers)
        endif()
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
endfunction()
nh_ingestion_dependencies()
