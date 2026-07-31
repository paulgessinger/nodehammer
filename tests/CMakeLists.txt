add_executable(nodehammer_tests
    test_main.cpp
    ir/test_semantic_ir.cpp
    ir/test_json_roundtrip.cpp
    ir/test_shape_variant.cpp
    ir/test_flatbuffer_roundtrip.cpp
    ir/test_render_flatbuffer_roundtrip.cpp
    config/test_config_loader.cpp
    config/test_config_writer.cpp
    config/test_config_validator.cpp
    config/test_config_fixtures.cpp
    config/test_predicate_parser.cpp
    import/test_synthetic_importer.cpp
    selection/test_predicate.cpp
    selection/test_selector.cpp
    tessellation/test_primitive_tessellator.cpp
    tessellation/test_boolean_tessellator.cpp
    tessellation/test_tessellation_pass.cpp
    tessellation/test_wedge_cut.cpp
    tessellation/test_build_pipeline.cpp
    export/test_export_resolve.cpp
    export/test_gltf_exporter.cpp
    export/test_render_exporter_registry.cpp
    export/test_semantic_exporter_registry.cpp
    viewer/test_bag_project_fs.cpp
    viewer/test_build_session.cpp
    viewer/test_byte_buffer.cpp
    viewer/test_app_state.cpp
    viewer/test_dynamic_render_scale.cpp
    test_scene_build.cpp
    test_compute_pipeline.cpp
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

# The Lua config front-end is compiled into nodehammer_lib, but only natively;
# its test follows the same gate.
if(NOT EMSCRIPTEN)
    target_sources(nodehammer_tests PRIVATE config/test_lua_config.cpp)
endif()

# ZipWorkingSet and the project manifest are unconditional in the core and
# cross-platform, so these run everywhere.
#
# miniz is PRIVATE to the core — no internal header names it — but these tests
# open the produced archives with miniz directly, so they declare it themselves.
target_sources(nodehammer_tests PRIVATE
    viewer/test_zip_working_set.cpp
    viewer/test_project_manifest.cpp
)
target_link_libraries(nodehammer_tests PRIVATE miniz::miniz)

# The archive tests exercise the native save-to-path internals (saveTo /
# writeBytesAtomic), so they stay gated native even though ArchiveProjectFs
# itself is cross-platform — but no longer gated on the viewer, since the
# archive stack is unconditional in the core.
if(NOT EMSCRIPTEN)
    target_sources(nodehammer_tests PRIVATE
        viewer/test_archive_project_fs.cpp
        viewer/test_archive_export.cpp
    )
endif()

# WatchedFilesystemProjectFs keeps both gates: wtr.watcher is still a
# viewer-only, native-only dependency (NH_APP_SOURCES).
if(NOT EMSCRIPTEN AND NODEHAMMER_WITH_VIEWER)
    target_sources(nodehammer_tests PRIVATE
        viewer/test_watched_filesystem_project_fs.cpp
    )
endif()

target_compile_definitions(nodehammer_tests PRIVATE
    NODEHAMMER_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures"
    $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

target_link_libraries(nodehammer_tests PRIVATE
    nodehammer_lib
    Catch2::Catch2WithMain
    # export/test_gltf_exporter.cpp includes <tiny_gltf.h> directly, to read back
    # what the exporter produced. tinygltf is PRIVATE to the core (nothing in an
    # internal header names it), so the test declares it rather than inheriting
    # it by accident.
    TinyGLTF::TinyGLTF
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
# Catch2's discovery script doesn't thread CMAKE_CROSSCOMPILING_EMULATOR
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
