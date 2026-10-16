add_executable(nodehammer_ingest_test core.cpp)
# Internal world-transform checks need implementation symbols that the SDK hides.
target_link_libraries(nodehammer_ingest_test PRIVATE nodehammer_ingest_objs
    glm::glm unordered_dense::unordered_dense zstd::libzstd_static ${NH_INGEST_BACKEND_DEPS})
target_compile_definitions(nodehammer_ingest_test PRIVATE NH_STATIC)
# These include our implementation headers. Marking them SYSTEM lets ccache
# with system_headers sloppiness reuse incompatible objects after header edits
# (for example, a changed inline ID hash). Keep them as normal dependencies.
target_include_directories(nodehammer_ingest_test PRIVATE
    "$<TARGET_PROPERTY:nodehammer_ingest_objs,INCLUDE_DIRECTORIES>")
set_target_properties(nodehammer_ingest_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
nh_apply_emscripten_exe_options(nodehammer_ingest_test)
if(EMSCRIPTEN)
    find_program(NODEHAMMER_NODE_EXECUTABLE node REQUIRED)
    add_test(NAME ingestion_core COMMAND "${NODEHAMMER_NODE_EXECUTABLE}"
        "$<TARGET_FILE:nodehammer_ingest_test>" "${CMAKE_CURRENT_BINARY_DIR}/sample.nhb")
else()
    add_test(NAME ingestion_core COMMAND nodehammer_ingest_test "${CMAKE_CURRENT_BINARY_DIR}/sample.nhb")
endif()

set_tests_properties(ingestion_core PROPERTIES FIXTURES_SETUP ingestion_sample)
if(NOT EMSCRIPTEN)
    add_executable(nodehammer_ingest_transport transport.cpp)
    target_link_libraries(nodehammer_ingest_transport PRIVATE nodehammer::ingest)
    set_target_properties(nodehammer_ingest_transport PROPERTIES CXX_STANDARD 20)
    nh_set_compiler_options(nodehammer_ingest_transport)
    add_test(NAME ingestion_transport COMMAND nodehammer_ingest_transport
        "${CMAKE_CURRENT_BINARY_DIR}/sample.nhb")
    set_tests_properties(ingestion_transport PROPERTIES FIXTURES_REQUIRED ingestion_sample)
    if(WIN32)
        set_tests_properties(ingestion_transport PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:nodehammer_ingest>")
    endif()
endif()

if(NODEHAMMER_WITH_TGEO)
    add_executable(nodehammer_ingest_geometry golden.cpp)
    target_link_libraries(nodehammer_ingest_geometry PRIVATE nodehammer::ingest ROOT::Geom)
    set_target_properties(nodehammer_ingest_geometry PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME ingestion_geometry COMMAND nodehammer_ingest_geometry
        ${CMAKE_CURRENT_BINARY_DIR}/geometry.nhb)

    if(TARGET nodehammer_lib)
        add_executable(nodehammer_full_geometry golden.cpp)
        target_link_libraries(nodehammer_full_geometry PRIVATE nodehammer_lib)
        add_test(NAME ingestion_equivalence COMMAND ${CMAKE_COMMAND}
            -DINGEST=$<TARGET_FILE:nodehammer_ingest_geometry>
            -DMODULAR=$<TARGET_FILE:nodehammer_full_geometry>
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/golden
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compare.cmake)
    endif()
endif()

if(NODEHAMMER_WITH_DD4HEP)
    add_executable(nodehammer_ingest_dd4hep dd4hep.cpp)
    target_link_libraries(nodehammer_ingest_dd4hep PRIVATE nodehammer::ingest DD4hep::DDCore)
    set_target_properties(nodehammer_ingest_dd4hep PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME ingestion_dd4hep COMMAND nodehammer_ingest_dd4hep
        ${PROJECT_SOURCE_DIR}/fixtures/dd4hep/simple_box.xml)
endif()

if(WIN32)
    foreach(test ingestion_geometry ingestion_dd4hep ingestion_equivalence)
        if(TEST ${test})
            set_tests_properties(${test} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:nodehammer_ingest>")
        endif()
    endforeach()
endif()

if(NODEHAMMER_WITH_GEOMODEL)
    add_executable(nodehammer_geomodel_check geomodel.cpp)
    target_link_libraries(nodehammer_geomodel_check PRIVATE nodehammer_ingest_objs
        glm::glm unordered_dense::unordered_dense zstd::libzstd_static ${NH_INGEST_BACKEND_DEPS})
    target_include_directories(nodehammer_geomodel_check PRIVATE
        "$<TARGET_PROPERTY:nodehammer_ingest_objs,INCLUDE_DIRECTORIES>")
    target_compile_definitions(nodehammer_geomodel_check PRIVATE NH_STATIC)
    set_target_properties(nodehammer_geomodel_check PROPERTIES CXX_STANDARD 20)
endif()
if(NODEHAMMER_WITH_GEOMODEL)
    add_executable(nodehammer_geomodel_geometry geomodel_geometry.cpp)
    target_link_libraries(nodehammer_geomodel_geometry PRIVATE nodehammer_ingest_objs
        glm::glm unordered_dense::unordered_dense zstd::libzstd_static ${NH_INGEST_BACKEND_DEPS}
        GeoModelIO::GeoModelWrite)
    target_include_directories(nodehammer_geomodel_geometry PRIVATE
        "$<TARGET_PROPERTY:nodehammer_ingest_objs,INCLUDE_DIRECTORIES>")
    target_compile_definitions(nodehammer_geomodel_geometry PRIVATE NH_STATIC)
    set_target_properties(nodehammer_geomodel_geometry PROPERTIES CXX_STANDARD 20)
    add_test(NAME ingestion_geomodel COMMAND nodehammer_geomodel_geometry "${CMAKE_CURRENT_BINARY_DIR}/geomodel-test.db")
endif()

if(NODEHAMMER_WITH_GEOMODEL)
    set_tests_properties(ingestion_geomodel PROPERTIES FIXTURES_SETUP geomodel_fixture)
    if(TARGET nodehammer)
        add_test(NAME geomodel_cli COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:nodehammer>
            -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/geomodel-test.db.opaque.db
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/geomodel-cli.nhb
            -P ${CMAKE_CURRENT_SOURCE_DIR}/geomodel_cli.cmake)
        set_tests_properties(geomodel_cli PROPERTIES FIXTURES_REQUIRED geomodel_fixture)
    endif()
endif()

if(NODEHAMMER_WITH_GEOMODEL)
    add_executable(nodehammer_geomodel_traps geomodel_traps.cpp)
    target_link_libraries(nodehammer_geomodel_traps PRIVATE nodehammer_ingest_objs
        glm::glm unordered_dense::unordered_dense zstd::libzstd_static ${NH_INGEST_BACKEND_DEPS}
        GeoModelIO::GeoModelWrite)
    target_include_directories(nodehammer_geomodel_traps PRIVATE
        "$<TARGET_PROPERTY:nodehammer_ingest_objs,INCLUDE_DIRECTORIES>")
    target_compile_definitions(nodehammer_geomodel_traps PRIVATE NH_STATIC)
    set_target_properties(nodehammer_geomodel_traps PROPERTIES CXX_STANDARD 20)
    add_test(NAME ingestion_geomodel_traps COMMAND nodehammer_geomodel_traps "${CMAKE_CURRENT_BINARY_DIR}/geomodel-traps.db")
endif()
if(NODEHAMMER_WITH_GEOMODEL)
    set_tests_properties(ingestion_geomodel_traps PROPERTIES FIXTURES_SETUP geomodel_traps_fixture)
    if(TARGET nodehammer)
        add_test(NAME geomodel_traps_cli COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:nodehammer>
            -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/geomodel-traps.db
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/geomodel-traps-cli.nhb
            -P ${CMAKE_CURRENT_SOURCE_DIR}/geomodel_traps_cli.cmake)
        set_tests_properties(geomodel_traps_cli PROPERTIES FIXTURES_REQUIRED geomodel_traps_fixture)
    endif()
endif()
