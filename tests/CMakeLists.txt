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
    api/test_api_surface.cpp
    api/test_api_equivalence.cpp
    api/test_config_wrapper.cpp
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

nh_set_compiler_options(nodehammer_tests)
nh_apply_emscripten_exe_options(nodehammer_tests)

# Tiny benchmark harness — `nodehammer_bench <cfg> <in> <out>`. Lives next to
# the test binary because it shares the same dependency surface (linked
# against nodehammer_lib only, no CLI11) and the same build/install
# expectations (NODERAWFS under emscripten so Node can read fixtures from
# the host filesystem). Driven by ci/bench.sh.
add_executable(nodehammer_bench bench_main.cpp)
target_link_libraries(nodehammer_bench PRIVATE nodehammer_lib)
nh_set_compiler_options(nodehammer_bench)
nh_apply_emscripten_exe_options(nodehammer_bench)

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

# ── The public-API suite ──────────────────────────────────────────────────────
#
# The same code as everything above, linked the other way: against
# nodehammer_shared rather than the archive. That single difference is the
# target's whole reason to exist.
#
# nodehammer_lib carries NH_STATIC as PUBLIC — which collapses NH_API to nothing
# — and nodehammer_shared carries NH_EXPORTS as PRIVATE, so a target linking the
# shared library inherits *neither*. It therefore sees the same NH_API spelling
# an installed consumer does (dllimport on Windows, default visibility
# elsewhere) and resolves against the same export table. A member that is
# missing its NH_API links fine everywhere else in this tree and fails here.
#
# ci/shared_consumer answers the neighbouring question — can somebody outside
# the build tree use an *installed* nodehammer, with no dependency hints and no
# source access — and it stays the authority on that. What it cannot be is a
# test suite: it is one `main()` with an exit code, so a broken entry point
# hides every entry point after it, and adding a case means adding another `if`
# and another error message. This is where the assertions go.
#
# Gated because there is nothing to link against otherwise. NODEHAMMER_BUILD_SHARED
# is a hard CMake error under Emscripten, so this is native-only by construction
# and ci/build.sh turns it on for every native leg.
if(NODEHAMMER_BUILD_SHARED)
    add_executable(nodehammer_public_tests
        public/test_public_build.cpp
        public/test_public_config.cpp
        public/test_public_diagnostics.cpp
        public/test_public_render_scene.cpp
        public/test_public_semantic_scene.cpp
    )

    # No nodehammer_lib, no fixtures directory, no TinyGLTF: this suite reaches
    # the library only through <nodehammer/*.hpp>, and it builds every input it
    # needs through the API itself. The include path still *reaches* src/ —
    # nodehammer_shared propagates it in-tree via nh_core_common — but an
    # internal header would not link, because internals are hidden in the shared
    # object. The rule is enforced by the linker rather than by review.
    target_link_libraries(nodehammer_public_tests PRIVATE
        nodehammer_shared
        Catch2::Catch2WithMain
    )

    # The one entry point in the public surface whose *export* is conditional:
    # `SemanticScene::read(TGeoManager &)` is declared in every build and defined
    # only where ROOT is present. Reaching it needs ROOT here too — a consumer
    # holding a TGeoManager necessarily has it, so linking it is what the real
    # call site looks like rather than a concession to the test.
    #
    # ROOT::Geom is already found at the top level under the same option, and
    # nodehammer_shared carries it PRIVATE, so this is the declaration that makes
    # the ROOT headers reachable from tests/public/ rather than a second lookup.
    if(NODEHAMMER_WITH_TGEO)
        target_sources(nodehammer_public_tests PRIVATE public/test_public_tgeo.cpp)
        target_link_libraries(nodehammer_public_tests PRIVATE ROOT::Geom)
    endif()

    nh_set_compiler_options(nodehammer_public_tests)

    include(Catch)
    # DL_PATHS puts the shared library on the loader's path for both the
    # discovery run and the tests themselves. Only Windows needs it — the DLL
    # sits in the runtime output directory with no rpath to record where it went
    # — but stating it unconditionally costs nothing and keeps the reason in one
    # place instead of behind an if(WIN32).
    catch_discover_tests(nodehammer_public_tests
        DL_PATHS $<TARGET_FILE_DIR:nodehammer_shared>
    )
endif()
