cmake_minimum_required(VERSION 3.25)

project(nodehammer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Dependencies.cmake)
include(cmake/CompilerOptions.cmake)

# ── FlatBuffers code generation ──────────────────────────────────────────────
set(NH_FBS_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/schemas/semantic.fbs)
set(NH_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers)

add_custom_command(
    OUTPUT ${NH_FBS_GENERATED_DIR}/semantic_generated.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NH_FBS_GENERATED_DIR}
    COMMAND flatc --cpp -o ${NH_FBS_GENERATED_DIR} ${NH_FBS_SCHEMA}
    DEPENDS ${NH_FBS_SCHEMA} flatc
    COMMENT "Generating FlatBuffers C++ header from semantic.fbs"
)
add_custom_target(flatbuffers_generate DEPENDS ${NH_FBS_GENERATED_DIR}/semantic_generated.h)

# ── Optional heavy backends ───────────────────────────────────────────────────
option(NODEHAMMER_WITH_TGEO    "Enable TGeo/ROOT importer"        OFF)
option(NODEHAMMER_WITH_DD4HEP  "Enable DD4hep importer"           OFF)
option(NODEHAMMER_WITH_GEANT4  "Enable GDML/Geant4 importer"      OFF)
option(NODEHAMMER_WITH_GEOMODEL "Enable GeoModel importer"        OFF)
option(NODEHAMMER_ENABLE_ASAN  "Enable AddressSanitizer"          OFF)

# ── Library target ─────────────────────────────────────────────────────────────
add_library(nodehammer_lib STATIC
    src/ir/semantic.cpp
    src/config/config_loader.cpp
    src/config/config_writer.cpp
    src/config/config_validator.cpp
    src/config/predicate_parser.cpp
    src/ir/synthetic/semantic/importer.cpp
    src/ir/semantic/importer_registry.cpp
    src/ir/json/semantic/importer.cpp
    src/ir/fb/semantic/importer.cpp
    src/ir/fb/semantic/flatbuffer.cpp
    src/selection/predicate.cpp
    src/selection/selector.cpp
    src/tessellation/primitive_tessellator.cpp
    src/tessellation/tessellation_pass.cpp
    src/ir/gltf/render/exporter.cpp
    src/ir/obj/render/exporter.cpp
    src/ir/render/exporter_registry.cpp
    src/ir/json/semantic/exporter.cpp
    src/ir/fb/semantic/exporter.cpp
    src/ir/semantic/exporter_registry.cpp
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
    zstd::libzstd_static
    flatbuffers
)

add_dependencies(nodehammer_lib flatbuffers_generate)
target_include_directories(nodehammer_lib SYSTEM PUBLIC ${NH_FBS_GENERATED_DIR})

nodehammer_set_compiler_options(nodehammer_lib)

# ── Optional backends ──────────────────────────────────────────────────────────
if(NODEHAMMER_WITH_TGEO OR NODEHAMMER_WITH_DD4HEP)
    find_package(ROOT REQUIRED COMPONENTS Geom)
    target_sources(nodehammer_lib PRIVATE src/ir/tgeo/semantic/shape_dispatch.cpp)
    target_link_libraries(nodehammer_lib PUBLIC ROOT::Geom)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_TGEO=1)
endif()

if(NODEHAMMER_WITH_TGEO)
    target_sources(nodehammer_lib PRIVATE src/ir/tgeo/semantic/importer.cpp)
endif()

if(NODEHAMMER_WITH_DD4HEP)
    find_package(DD4hep REQUIRED)
    target_sources(nodehammer_lib PRIVATE src/ir/dd4hep/semantic/importer.cpp)
    target_link_libraries(nodehammer_lib PUBLIC DD4hep::DDCore)
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_DD4HEP=1)
endif()

if(NODEHAMMER_WITH_GEANT4)
    find_package(Geant4 REQUIRED)
    target_link_libraries(nodehammer_lib PUBLIC ${Geant4_LIBRARIES})
    target_include_directories(nodehammer_lib PUBLIC ${Geant4_INCLUDE_DIRS})
    target_compile_definitions(nodehammer_lib PUBLIC NH_WITH_GEANT4=1)
endif()

target_sources(nodehammer_lib PRIVATE src/tessellation/boolean_tessellator.cpp)
target_link_libraries(nodehammer_lib PUBLIC manifold::manifold)

# ── CLI executable ─────────────────────────────────────────────────────────────
add_executable(nodehammer src/cli/main.cpp)
target_link_libraries(nodehammer PRIVATE nodehammer_lib)
nodehammer_set_compiler_options(nodehammer)

# ── Tests ──────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)
