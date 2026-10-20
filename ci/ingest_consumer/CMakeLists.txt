cmake_minimum_required(VERSION 3.25)
project(ingestion_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(CONSUMER_WITH_TGEO "Use the experiment's ROOT installation" OFF)
option(CONSUMER_WITH_DD4HEP "Use the experiment's DD4hep installation" OFF)
option(CONSUMER_WITH_GEOMODEL "Use the experiment's GeoModel installation" OFF)
if(CONSUMER_WITH_GEOMODEL AND NOT (NODEHAMMER_SOURCE_DIR AND NODEHAMMER_FETCH_GEOMODEL))
    find_package(GeoModelCore 6.29.0 CONFIG REQUIRED)
    find_package(GeoModelIO 6.29.0 CONFIG REQUIRED)
endif()
if(CONSUMER_WITH_DD4HEP)
    set(CONSUMER_WITH_TGEO ON)
    find_package(DD4hep REQUIRED)
endif()
if(CONSUMER_WITH_TGEO)
    find_package(ROOT REQUIRED COMPONENTS Geom)
endif()

if(NODEHAMMER_SOURCE_DIR)
    # Stand-in for FetchContent_MakeAvailable(nodehammer). The parent resolves
    # its backends first, and nodehammer must reuse their imported targets.
    set(NODEHAMMER_INGEST_ONLY ON)
    set(NODEHAMMER_WITH_TGEO ${CONSUMER_WITH_TGEO})
    set(NODEHAMMER_WITH_DD4HEP ${CONSUMER_WITH_DD4HEP})
    set(NODEHAMMER_WITH_GEOMODEL ${CONSUMER_WITH_GEOMODEL})
    add_subdirectory(${NODEHAMMER_SOURCE_DIR} nodehammer)
else()
    find_package(nodehammer CONFIG REQUIRED COMPONENTS Ingest)
endif()

# DD4hep may itself expose nlohmann_json (through EDM4HEP/podio). Check
# nodehammer's downstream processing targets, not the backend's dependencies.
foreach(forbidden nodehammer_lib nodehammer_shared manifold manifold::manifold
        CLI11::CLI11 lua::lua)
    if(TARGET ${forbidden})
        message(FATAL_ERROR "Ingestion configured a downstream target: ${forbidden}")
    endif()
endforeach()
if(NOT CMAKE_CXX_STANDARD EQUAL 20)
    message(FATAL_ERROR "Embedding nodehammer changed the parent's C++ standard")
endif()

add_executable(consumer main.cpp)
# Verify the current SDK even when CI's ccache skips system-header changes.
# Imported targets otherwise make the installed headers system includes, so
# a cached consumer can retain an older VERSION after the SDK is rebuilt.
set_target_properties(consumer PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
target_link_libraries(consumer PRIVATE nodehammer::ingest)
if(NODEHAMMER_SOURCE_DIR)
    target_compile_definitions(consumer PRIVATE CONSUMER_MINIMAL_HEADERS)
endif()
if(CONSUMER_WITH_TGEO)
    target_compile_definitions(consumer PRIVATE CONSUMER_WITH_TGEO)
    target_link_libraries(consumer PRIVATE ROOT::Geom)
endif()
enable_testing()
add_test(NAME external_ingestion COMMAND consumer)

if(WIN32 AND NODEHAMMER_SOURCE_DIR)
    set_tests_properties(external_ingestion PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:nodehammer_ingest>")
endif()
