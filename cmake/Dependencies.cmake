include(FetchContent)

# ── Catch2 v3 ─────────────────────────────────────────────────────────────────
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    FIND_PACKAGE_ARGS 3.7.1
)
FetchContent_MakeAvailable(Catch2)

# ── CLI11 ─────────────────────────────────────────────────────────────────────
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    FIND_PACKAGE_ARGS 2.4.2
)
FetchContent_MakeAvailable(CLI11)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    FIND_PACKAGE_ARGS 3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# ── GLM ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    FIND_PACKAGE_ARGS 1.0.1
)
FetchContent_MakeAvailable(glm)

# ── toml++ ────────────────────────────────────────────────────────────────────
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    FIND_PACKAGE_ARGS 3.4.0
)
FetchContent_MakeAvailable(tomlplusplus)

# ── tinygltf ──────────────────────────────────────────────────────────────────
# Header-only glTF 2.0 reader/writer. Uses nlohmann_json (made available above).
FetchContent_Declare(tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG        v2.9.7
    FIND_PACKAGE_ARGS 2.9.7
)

# Disable tinygltf's own examples/tests
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE BOOL "" FORCE)
set(TINYGLTF_INSTALL               OFF CACHE BOOL "" FORCE)
set(TINYGLTF_HEADER_ONLY           ON  CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(tinygltf)
