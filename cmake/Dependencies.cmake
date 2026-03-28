include(FetchContent)

# ── Catch2 v3 ─────────────────────────────────────────────────────────────────
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
)

# ── CLI11 ─────────────────────────────────────────────────────────────────────
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)

# ── GLM ───────────────────────────────────────────────────────────────────────
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)

# ── toml++ ────────────────────────────────────────────────────────────────────
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
)

# ── fx-gltf ───────────────────────────────────────────────────────────────────
# fx-gltf requires nlohmann_json; fetch before MakeAvailable so it's found.
FetchContent_Declare(fx-gltf
    GIT_REPOSITORY https://github.com/jessey-git/fx-gltf.git
    GIT_TAG        v2.0.0
)

# Disable fx-gltf's own test/sample targets to avoid pulling in extra deps
set(FX_GLTF_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FX_GLTF_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(
    Catch2
    CLI11
    nlohmann_json
    glm
    tomlplusplus
    fx-gltf
)
