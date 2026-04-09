cmake_minimum_required(VERSION 3.25)

project(nodehammer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Dependencies.cmake)
include(cmake/CompilerOptions.cmake)

# ── Optional heavy backends ───────────────────────────────────────────────────
option(NODEHAMMER_WITH_TGEO    "Enable TGeo/ROOT importer"        OFF)
option(NODEHAMMER_WITH_DD4HEP  "Enable DD4hep importer"           OFF)
option(NODEHAMMER_WITH_GEANT4  "Enable GDML/Geant4 importer"      OFF)
option(NODEHAMMER_WITH_GEOMODEL "Enable GeoModel importer"        OFF)
option(NODEHAMMER_WITH_BOOLEAN_MESH "Enable Manifold boolean mesh" OFF)
option(NODEHAMMER_ENABLE_ASAN  "Enable AddressSanitizer"          OFF)

# ── Library target ─────────────────────────────────────────────────────────────
add_library(nodehammer_lib STATIC
    src/ir/semantic.cpp
    src/config/config_loader.cpp
    src/config/config_validator.cpp
    src/config/predicate_parser.cpp
    src/import/synthetic.cpp
    src/import/importer_registry.cpp
    src/selection/predicate.cpp
    src/selection/selector.cpp
    src/selection/closure.cpp
    src/tessellation/primitive_tessellator.cpp
    src/tessellation/tessellation_pass.cpp
    src/export/gltf_exporter.cpp
    src/export/obj_exporter.cpp
    src/export/exporter_registry.cpp
    src/markup.cpp
    src/cli/cmd_convert.cpp
    src/cli/cmd_inspect.cpp
    src/cli/cmd_validate_config.cpp
    src/cli/cmd_dump_semantic.cpp
    src/cli/cmd_dump_render.cpp
)

target_include_directories(nodehammer_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(nodehammer_lib PUBLIC
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    glm::glm
    tinygltf
    CLI11::CLI11
)

nodehammer_set_compiler_options(nodehammer_lib)

# ── Optional backends ──────────────────────────────────────────────────────────
if(NODEHAMMER_WITH_TGEO OR NODEHAMMER_WITH_DD4HEP)
    find_package(ROOT REQUIRED COMPONENTS Geom)
    target_sources(nodehammer_lib PRIVATE src/import/tgeo/tgeo_shape_dispatch.cpp)
    target_link_libraries(nodehammer_lib PUBLIC ROOT::Geom)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_TGEO=1)
endif()

if(NODEHAMMER_WITH_TGEO)
    target_sources(nodehammer_lib PRIVATE src/import/tgeo/tgeo_importer.cpp)
endif()

if(NODEHAMMER_WITH_DD4HEP)
    find_package(DD4hep REQUIRED)
    target_sources(nodehammer_lib PRIVATE src/import/dd4hep/dd4hep_importer.cpp)
    target_link_libraries(nodehammer_lib PUBLIC DD4hep::DDCore)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_DD4HEP=1)
endif()

if(NODEHAMMER_WITH_GEANT4)
    find_package(Geant4 REQUIRED)
    target_sources(nodehammer_lib PRIVATE
        src/import/gdml/gdml_importer.cpp
    )
    target_link_libraries(nodehammer_lib PUBLIC ${Geant4_LIBRARIES})
    target_include_directories(nodehammer_lib PUBLIC ${Geant4_INCLUDE_DIRS})
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_GEANT4=1)
endif()

# ── CLI executable ─────────────────────────────────────────────────────────────
add_executable(nodehammer src/cli/main.cpp)
target_link_libraries(nodehammer PRIVATE nodehammer_lib)

# ── Tests ──────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
