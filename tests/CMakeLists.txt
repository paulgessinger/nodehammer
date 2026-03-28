add_executable(nodehammer_tests
    test_main.cpp
)

target_link_libraries(nodehammer_tests PRIVATE
    nodehammer_lib
    Catch2::Catch2WithMain
)

nodehammer_set_compiler_options(nodehammer_tests)

include(CTest)
include(Catch)
catch_discover_tests(nodehammer_tests)
