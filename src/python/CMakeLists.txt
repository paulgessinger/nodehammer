# The nanobind extension module.
#
# Linked the same way tests/public/ is — against nodehammer_shared, never the
# archive — so the boundary is enforced by the linker rather than by review:
# nodehammer_lib carries NH_STATIC PUBLIC and nodehammer_shared carries
# NH_EXPORTS PRIVATE, so this target inherits neither and sees the NH_API
# spelling an installed consumer sees. An internal header is still *includable*
# in-tree (the shared target propagates src/ through BUILD_INTERFACE) but its
# symbols are hidden in the shared object, so it does not link. That asymmetry
# is the point: it makes "the bindings use only the public API" a fact about the
# build rather than a rule someone has to remember.

# SKBUILD_SABI_COMPONENT is "Development.SABIModule" when scikit-build-core is
# driving an abi3 wheel and empty otherwise, so this one line covers both the
# wheel build and a plain developer configure.
find_package(Python 3.12 REQUIRED
             COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT})

# nanobind ships its CMake config inside its PyPI wheel — there is no Conan
# recipe and no FetchContent entry — so it is located through the interpreter
# that is building the extension. Under scikit-build-core that is the isolated
# build environment, which has nanobind from build-system.requires.
if(NOT DEFINED nanobind_ROOT)
    execute_process(
        COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        OUTPUT_VARIABLE nanobind_ROOT
        RESULT_VARIABLE _nh_nanobind_result
        ERROR_VARIABLE _nh_nanobind_error)
    if(NOT _nh_nanobind_result EQUAL 0)
        message(FATAL_ERROR
                "NODEHAMMER_BUILD_PYTHON is ON but nanobind is not importable from "
                "${Python_EXECUTABLE}:\n${_nh_nanobind_error}\n"
                "Install it (`pip install nanobind`), or build the wheel with "
                "`just wheel`, which takes it from build-system.requires.")
    endif()
endif()
find_package(nanobind CONFIG REQUIRED)

message(STATUS "nodehammer: python extension against ${Python_EXECUTABLE}")

# NB_STATIC: one extension module in this project, so nanobind's runtime is
# absorbed rather than shipped as a second shared object the wheel would have to
# place and rpath.
#
# STABLE_ABI: abi3, so one wheel covers 3.12 and everything after it. nanobind
# *silently ignores* this when Development.SABIModule was not found, which is
# why tests/python asserts the resulting filename rather than trusting it.
nanobind_add_module(nodehammer_python
    NB_STATIC
    STABLE_ABI
    bindings.cpp
)

set_target_properties(nodehammer_python PROPERTIES OUTPUT_NAME _nodehammer)

target_link_libraries(nodehammer_python PRIVATE nodehammer_shared)

# nodehammer_shared states cxx_std_20 INTERFACE — the floor an *external*
# consumer must meet, not what this tree builds with. These bindings are in-tree
# code and are built like the rest of it.
target_compile_features(nodehammer_python PRIVATE cxx_std_23)

nh_set_compiler_options(nodehammer_python)
nh_set_visibility(nodehammer_python)

# GCC's -Wshadow flags a lambda parameter that shadows an enclosing local even
# when nothing captures it; clang does not, unless asked. This file is almost
# entirely lambdas closing over a scope full of locals, so that divergence means
# a clean build here and a -Werror failure on the Linux legs -- which is exactly
# how it was found. Asking clang for the same check keeps the two honest.
#
# Scoped to this target rather than the whole tree: src/selection/predicate.cpp
# trips clang's version in three places that GCC does not object to, so the flags
# are near-equivalents rather than equivalents, and widening it is a separate
# question from keeping the bindings buildable everywhere.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(nodehammer_python PRIVATE -Wshadow-uncaptured-local)
endif()

# nh_set_compiler_options turns on -Wall -Wextra -Wpedantic -Wconversion and
# friends, which NODEHAMMER_WERROR then makes fatal. nanobind does not mark its
# own interface includes SYSTEM, so those warnings fire inside its headers —
# -Warray-bounds on nb_func.h's argument-pack indexing, which is correct code
# the compiler cannot see through. Dropping the helper would exempt *our*
# bindings from the project's warning set, so mark the dependency's headers
# system instead: our code stays held to the same standard as the rest of the
# tree, and nanobind's is not our diagnostic to fix.
get_target_property(_nh_nb_links nodehammer_python LINK_LIBRARIES)
foreach(_nh_nb_lib IN LISTS _nh_nb_links)
    if(TARGET ${_nh_nb_lib} AND _nh_nb_lib MATCHES "^nanobind")
        get_target_property(_nh_nb_inc ${_nh_nb_lib} INTERFACE_INCLUDE_DIRECTORIES)
        if(_nh_nb_inc)
            set_target_properties(${_nh_nb_lib} PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_nh_nb_inc}")
        endif()
    endif()
endforeach()

# One relative rpath entry, and nothing else.
#
# CMAKE_INSTALL_RPATH_USE_LINK_PATH is ON at the top level for the ROOT/DD4hep
# case, where a DT_NEEDED reference has to be findable on the machine that built
# it. A wheel is the opposite situation: every dependency is a static archive
# absorbed into libnodehammer, there is nothing left to point at, and an
# absolute build-machine path in RUNPATH is the one thing auditwheel and
# delocate cannot repair. The sibling entry is also what makes those tools
# classify the bundled library as already-internal and leave its SONAME alone.
set_target_properties(nodehammer_python PROPERTIES
    INSTALL_RPATH_USE_LINK_PATH OFF
    INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>"
)

# A usable package in the build tree, so `import nodehammer` works before a
# wheel exists: the extension is built straight into it and the pure-Python half
# is copied beside it, giving the same directory layout the wheel will have.
# Tests then run against `PYTHONPATH=<build>/python` and exercise the real
# package rather than a bare extension module.
#
# The shared library is *not* copied here — in the build tree CMake's own build
# rpath already resolves it. Whether the relative rpath works is a property of
# the installed layout, so it is checked on the wheel, where it is true.
set(NH_PYTHON_BUILD_PKG "${CMAKE_BINARY_DIR}/python/nodehammer")
set_target_properties(nodehammer_python PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${NH_PYTHON_BUILD_PKG}"
)

foreach(_nh_py_file __init__.py py.typed)
    add_custom_command(
        OUTPUT "${NH_PYTHON_BUILD_PKG}/${_nh_py_file}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/python/nodehammer/${_nh_py_file}"
                "${NH_PYTHON_BUILD_PKG}/${_nh_py_file}"
        DEPENDS "${CMAKE_SOURCE_DIR}/python/nodehammer/${_nh_py_file}"
        COMMENT "Staging python/nodehammer/${_nh_py_file}"
        VERBATIM)
    list(APPEND _nh_py_staged "${NH_PYTHON_BUILD_PKG}/${_nh_py_file}")
endforeach()

add_custom_target(nodehammer_python_package ALL DEPENDS ${_nh_py_staged})
add_dependencies(nodehammer_python_package nodehammer_python nodehammer_python_stub)

# Under scikit-build-core CMAKE_INSTALL_PREFIX is the wheel's platlib staging
# directory, so a bare relative path is the package directory. Outside it this
# is relative to whatever prefix the caller passed — meaningless, but also
# unreachable, since nothing but the wheel build installs the Python component.
set(NH_PYTHON_PKG_DIR "nodehammer" CACHE INTERNAL "Wheel package directory")

install(TARGETS nodehammer_python
    LIBRARY DESTINATION "${NH_PYTHON_PKG_DIR}"
            COMPONENT Python
)

# Type stubs, generated by importing the module that was just built.
#
# Without these the py.typed marker beside them is a promise the package does not
# keep: it tells a type checker "this package is annotated", and the checker then
# finds an extension module with no annotations at all. The stub is also the only
# place the property-versus-method distinction is published — `valid: bool` reads
# differently from `def valid(self) -> bool`, and a checker is what turns that
# into an error at the call site rather than at runtime.
nanobind_add_stub(nodehammer_python_stub
    MODULE _nodehammer
    OUTPUT "${NH_PYTHON_BUILD_PKG}/_nodehammer.pyi"
    PYTHON_PATH "${NH_PYTHON_BUILD_PKG}"
    DEPENDS nodehammer_python
    VERBOSE
)

install(FILES "${NH_PYTHON_BUILD_PKG}/_nodehammer.pyi"
    DESTINATION "${NH_PYTHON_PKG_DIR}"
    COMPONENT Python
)

# The pytest mirror of tests/public/, registered as an ordinary ctest test.
#
# Registered here rather than in tests/CMakeLists.txt because Python_EXECUTABLE
# is found in this directory, and because the suite belongs with the module it
# exercises. The payoff is that it needs no CI wiring of its own: it joins the
# `ctest --preset` invocation every native job already runs, and shows up in the
# same count as the C++ tests.
#
# Deliberately not guarded on pytest being importable. A configuration that
# turned this on and then skipped the tests because a dependency was missing
# would be indistinguishable from one where they passed -- and the whole reason
# this exists is that the bindings were previously compiled and tested nowhere
# but a developer's laptop.
if(NODEHAMMER_BUILD_TESTS)
    add_test(NAME python_bindings
             COMMAND "${Python_EXECUTABLE}" -m pytest "${CMAKE_SOURCE_DIR}/tests/python" -q)
    set_tests_properties(python_bindings PROPERTIES
        ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python")
endif()
