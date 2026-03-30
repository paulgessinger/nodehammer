add_executable(nodehammer_tests
    test_main.cpp
    ir/test_semantic_ir.cpp
    ir/test_json_roundtrip.cpp
    ir/test_shape_variant.cpp
    config/test_config_loader.cpp
    config/test_config_validator.cpp
    import/test_synthetic_importer.cpp
)

target_compile_definitions(nodehammer_tests PRIVATE
    NODEHAMMER_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/fixtures"
)

target_link_libraries(nodehammer_tests PRIVATE
    nodehammer_lib
    Catch2::Catch2WithMain
)

nodehammer_set_compiler_options(nodehammer_tests)

include(CTest)
include(Catch)
catch_discover_tests(nodehammer_tests)
