add_executable(nodehammer_tests
    test_main.cpp
    ir/test_semantic_ir.cpp
    ir/test_json_roundtrip.cpp
    ir/test_shape_variant.cpp
    ir/test_flatbuffer_roundtrip.cpp
    config/test_config_loader.cpp
    config/test_config_writer.cpp
    config/test_config_validator.cpp
    config/test_predicate_parser.cpp
    import/test_synthetic_importer.cpp
    selection/test_predicate.cpp
    selection/test_selector.cpp
    tessellation/test_primitive_tessellator.cpp
    tessellation/test_tessellation_pass.cpp
    export/test_gltf_exporter.cpp
    export/test_render_exporter_registry.cpp
    export/test_semantic_exporter_registry.cpp
    viewer/test_bag_project_fs.cpp
    viewer/test_build_session.cpp
    viewer/test_byte_buffer.cpp
    viewer/test_app_state.cpp
    test_markup.cpp
    test_timing.cpp
)

# FilesystemProjectFs and NativeBagProjectFs are native-only; the
# sources are gated the same way in nodehammer_lib. Tests follow.
if(NOT EMSCRIPTEN)
    target_sources(nodehammer_tests PRIVATE
        viewer/test_filesystem_project_fs.cpp
        viewer/test_native_bag_project_fs.cpp
    )
endif()

target_compile_definitions(nodehammer_tests PRIVATE
    NODEHAMMER_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures"
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(nodehammer_tests PRIVATE
    nodehammer_lib
    Catch2::Catch2WithMain
)

if(NODEHAMMER_WITH_TGEO OR NODEHAMMER_WITH_DD4HEP)
    target_compile_definitions(nodehammer_tests PRIVATE NH_WITH_TGEO=1)
endif()

if(NODEHAMMER_WITH_TGEO)
    target_sources(nodehammer_tests PRIVATE import/test_tgeo_importer.cpp)
    target_sources(nodehammer_tests PRIVATE tessellation/test_tgeo_tessellation.cpp)
    target_compile_definitions(nodehammer_tests PRIVATE
        NODEHAMMER_TESS_OBJ_DIR="${CMAKE_BINARY_DIR}/tessellation_obj"
    )
endif()

if(NODEHAMMER_WITH_DD4HEP)
    target_sources(nodehammer_tests PRIVATE import/test_dd4hep_importer.cpp)
    target_compile_definitions(nodehammer_tests PRIVATE NH_WITH_DD4HEP=1)
endif()

nodehammer_set_compiler_options(nodehammer_tests)
nodehammer_apply_emscripten_exe_options(nodehammer_tests)

# Tiny benchmark harness — `nodehammer_bench <cfg> <in> <out>`. Lives next to
# the test binary because it shares the same dependency surface (linked
# against nodehammer_lib only, no CLI11) and the same build/install
# expectations (NODERAWFS under emscripten so Node can read fixtures from
# the host filesystem). Driven by ci/bench.sh.
add_executable(nodehammer_bench bench_main.cpp)
target_link_libraries(nodehammer_bench PRIVATE nodehammer_lib)
nodehammer_set_compiler_options(nodehammer_bench)
nodehammer_apply_emscripten_exe_options(nodehammer_bench)

include(CTest)
# catch_discover_tests runs the test binary to enumerate cases. Under
# cross-compile the binary is a .js file that needs `node` as emulator, and
# Catch2 3.7.1's discovery script doesn't thread CMAKE_CROSSCOMPILING_EMULATOR
# through every path — it tries to execve the .js and gets EACCES. Register a
# single ctest entry invoking node directly. Catch2's own runner still prints
# per-case results on stdout.
if(CMAKE_CROSSCOMPILING)
    find_program(NODEHAMMER_NODE_EXECUTABLE node REQUIRED)
    add_test(
        NAME nodehammer_tests
        COMMAND "${NODEHAMMER_NODE_EXECUTABLE}" "$<TARGET_FILE:nodehammer_tests>"
    )
    add_test(
        NAME nodehammer_bench_smoke
        COMMAND "${NODEHAMMER_NODE_EXECUTABLE}" "$<TARGET_FILE:nodehammer_bench>"
                "${CMAKE_SOURCE_DIR}/fixtures/configs/odd.toml"
                "${CMAKE_SOURCE_DIR}/odd.nhb.zst"
                "${CMAKE_CURRENT_BINARY_DIR}/bench_smoke.glb"
    )
else()
    include(Catch)
    catch_discover_tests(nodehammer_tests)
    add_test(
        NAME nodehammer_bench_smoke
        COMMAND nodehammer_bench
                "${CMAKE_SOURCE_DIR}/fixtures/configs/odd.toml"
                "${CMAKE_SOURCE_DIR}/odd.nhb.zst"
                "${CMAKE_CURRENT_BINARY_DIR}/bench_smoke.glb"
    )
endif()
