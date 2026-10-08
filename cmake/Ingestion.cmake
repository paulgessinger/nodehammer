# Common source/target definition for the minimal and full builds. Full
# archives and shared libraries consume these objects, not a second compilation.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nodehammer/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp @ONLY)

# Expose only ingestion headers to source consumers, matching the minimal install.
foreach(_nh_header semantic_scene.hpp nhb.hpp diagnostics.hpp visibility.hpp)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/include/nodehammer/${_nh_header}"
        "${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/${_nh_header}" COPYONLY)
endforeach()

set(NH_INGEST_SOURCES
    src/version.cpp
    src/api/diagnostics.cpp
    src/api/semantic_scene.cpp
    src/ir/semantic.cpp
    src/ir/expanded/scene.cpp
    src/ir/fb/semantic/flatbuffer.cpp
    src/ir/fb/semantic/importer.cpp
)
set(NH_INGEST_BACKEND_DEPS "")
set(NH_INGEST_DEFINES "")
if(NODEHAMMER_WITH_TGEO)
    if(NOT TARGET ROOT::Geom)
        find_package(ROOT REQUIRED COMPONENTS Geom)
    endif()
    list(APPEND NH_INGEST_SOURCES
        src/ir/tgeo/semantic/importer.cpp
        src/ir/tgeo/semantic/shape_dispatch.cpp)
    list(APPEND NH_INGEST_BACKEND_DEPS ROOT::Geom)
    list(APPEND NH_INGEST_DEFINES NH_WITH_TGEO=1)
endif()
if(NODEHAMMER_WITH_DD4HEP)
    if(NOT TARGET DD4hep::DDCore)
        find_package(DD4hep REQUIRED)
    endif()
    list(APPEND NH_INGEST_SOURCES src/ir/dd4hep/semantic/importer.cpp)
    list(APPEND NH_INGEST_BACKEND_DEPS DD4hep::DDCore)
    list(APPEND NH_INGEST_DEFINES NH_WITH_DD4HEP=1)
endif()

set(NH_FBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/flatbuffers)
if(TARGET flatbuffers::flatc)
    set(NH_INGEST_FLATC flatbuffers::flatc)
elseif(TARGET flatc AND NOT CMAKE_CROSSCOMPILING)
    set(NH_INGEST_FLATC flatc)
else()
    find_program(NH_INGEST_FLATC_EXE flatc REQUIRED)
    set(NH_INGEST_FLATC "${NH_INGEST_FLATC_EXE}")
endif()
set(NH_SEMANTIC_GENERATED ${NH_FBS_GENERATED_DIR}/semantic_generated.h)
add_custom_command(OUTPUT ${NH_SEMANTIC_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${NH_FBS_GENERATED_DIR}
    COMMAND ${NH_INGEST_FLATC} --cpp -o ${NH_FBS_GENERATED_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/schemas/semantic.fbs
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/schemas/semantic.fbs ${NH_INGEST_FLATC}
    VERBATIM)
add_custom_target(nodehammer_semantic_generate DEPENDS ${NH_SEMANTIC_GENERATED})

function(nh_ingest_objects target)
    add_library(${target} OBJECT EXCLUDE_FROM_ALL ${NH_INGEST_SOURCES})
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(NOT EMSCRIPTEN)
        set_property(TARGET ${target} PROPERTY POSITION_INDEPENDENT_CODE ON)
    endif()
    add_dependencies(${target} nodehammer_semantic_generate)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_include_directories(${target} SYSTEM PRIVATE ${NH_FBS_GENERATED_DIR})
    # Retain header packages' transitive compile requirements (including any
    # definitions supplied by the embedding project). The object consumers
    # declare their own link dependencies, so these stay implementation-only.
    target_link_libraries(${target} PRIVATE glm::glm
        unordered_dense::unordered_dense zstd::libzstd_static ${NH_INGEST_BACKEND_DEPS})
    # Generated codecs only need FlatBuffers headers, not its parser archive.
    target_include_directories(${target} SYSTEM PRIVATE
        "$<TARGET_PROPERTY:flatbuffers::flatbuffers,INTERFACE_INCLUDE_DIRECTORIES>")
    target_compile_definitions(${target} PRIVATE
        "$<TARGET_PROPERTY:flatbuffers::flatbuffers,INTERFACE_COMPILE_DEFINITIONS>")
    target_compile_definitions(${target} PRIVATE ${NH_INGEST_DEFINES})
    nh_set_compiler_options(${target})
    nh_set_visibility(${target})
endfunction()

nh_ingest_objects(nodehammer_ingest_objs)
if(WIN32 OR EMSCRIPTEN)
    target_compile_definitions(nodehammer_ingest_objs PRIVATE NH_STATIC)
endif()
set(NH_INGEST_SHARED_OBJECTS nodehammer_ingest_objs)
if(WIN32 AND NOT EMSCRIPTEN)
    nh_ingest_objects(nodehammer_ingest_shared_objs)
    target_compile_definitions(nodehammer_ingest_shared_objs PRIVATE NH_EXPORTS)
    set(NH_INGEST_SHARED_OBJECTS nodehammer_ingest_shared_objs)
endif()

# The full libraries consume the objects directly; none depends on this DSO.
# Emscripten only uses those objects in its statically linked modules.
if(NOT EMSCRIPTEN)
    add_library(nodehammer_ingest SHARED EXCLUDE_FROM_ALL
        $<TARGET_OBJECTS:${NH_INGEST_SHARED_OBJECTS}>)
    add_library(nodehammer::ingest ALIAS nodehammer_ingest)
    nh_set_compiler_options(nodehammer_ingest)
    nh_set_visibility(nodehammer_ingest)
    if(NODEHAMMER_INGEST_ONLY OR NODEHAMMER_BUILD_SHARED)
        set_property(TARGET nodehammer_ingest PROPERTY EXCLUDE_FROM_ALL OFF)
    endif()
    set_target_properties(nodehammer_ingest PROPERTIES
        EXPORT_NAME ingest VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
        INSTALL_RPATH_USE_LINK_PATH ON)
    target_compile_features(nodehammer_ingest INTERFACE cxx_std_20)
    target_include_directories(nodehammer_ingest INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    # Absorb static zstd; experiment backends remain shared runtime dependencies.
    target_link_libraries(nodehammer_ingest PRIVATE zstd::libzstd_static ${NH_INGEST_BACKEND_DEPS})
    if(UNIX AND NOT APPLE)
        target_link_options(nodehammer_ingest PRIVATE
            "LINKER:--exclude-libs,ALL" "LINKER:--no-undefined" "LINKER:--enable-new-dtags")
    endif()
endif()

# Wheel builds select only the Python target and component, so they do not
# build or package the standalone ingestion DSO or the SDK.
if(NODEHAMMER_INGEST_ONLY OR NODEHAMMER_BUILD_SHARED)
    install(TARGETS nodehammer_ingest EXPORT nodehammer-ingest-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime NAMELINK_COMPONENT Development
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development)
    install(FILES
        include/nodehammer/semantic_scene.hpp
        include/nodehammer/nhb.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/include/nodehammer/version.hpp
        include/nodehammer/diagnostics.hpp
        include/nodehammer/visibility.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nodehammer COMPONENT Development)
    install(EXPORT nodehammer-ingest-targets NAMESPACE nodehammer::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer COMPONENT Development)
    include(${CMAKE_CURRENT_LIST_DIR}/InstallPackage.cmake)
endif()
if(NODEHAMMER_INGEST_ONLY)
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nodehammer COMPONENT Runtime)
endif()
