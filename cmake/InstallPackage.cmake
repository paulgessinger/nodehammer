# Shared by minimal and full SDK installs; targets have separate export files
# so finding one component does not load the other one's dependency graph.
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/nodehammer-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer)
# Pre-1.0: a matching major version alone does not promise ABI compatibility.
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/nodehammer-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer COMPONENT Development)

# Both SDK libraries use the same C++ runtime. Resolve generator expressions
# before installation, as the shared API passes owning std:: values across it.
if(MSVC)
    set(_nh_runtime_stamp "${CMAKE_CURRENT_BINARY_DIR}/nodehammer-runtime.txt")
    file(GENERATE OUTPUT "${_nh_runtime_stamp}"
        CONTENT "$<TARGET_PROPERTY:nodehammer_ingest,MSVC_RUNTIME_LIBRARY>")
    install(FILES "${_nh_runtime_stamp}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nodehammer COMPONENT Development)
endif()
