include(FetchContent)

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

# ── Manifold (boolean mesh operations) ─────────────────────────────────────────
FetchContent_Declare(manifold
        SYSTEM
        GIT_REPOSITORY https://github.com/elalish/manifold.git
        GIT_TAG        v3.0.1
        FIND_PACKAGE_ARGS 3.0
    )
    set(MANIFOLD_TEST OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_PYBIND OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_CBIND OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_JSBIND OFF CACHE BOOL "" FORCE)
    set(MANIFOLD_EXPORT OFF CACHE BOOL "" FORCE)
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
