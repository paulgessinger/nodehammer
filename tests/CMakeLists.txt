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

if(NODEHAMMER_WITH_TGEO OR NODEHAMMER_WITH_DD4HEP)
    target_compile_definitions(nodehammer_tests PRIVATE NH_WITH_TGEO=1)
endif()

if(NODEHAMMER_WITH_TGEO)
    target_sources(nodehammer_tests PRIVATE import/test_tgeo_importer.cpp)
endif()

if(NODEHAMMER_WITH_DD4HEP)
    target_sources(nodehammer_tests PRIVATE import/test_dd4hep_importer.cpp)
    target_compile_definitions(nodehammer_tests PRIVATE NH_WITH_DD4HEP=1)
endif()

nodehammer_set_compiler_options(nodehammer_tests)

include(CTest)
include(Catch)
catch_discover_tests(nodehammer_tests)
