# Stand-in for an external consumer, driven by ci/verify-shared-install.sh. What
# is under test is mostly everything this file does *not* say:
#
#   - no find_dependency, no CMAKE_PREFIX_PATH beyond nodehammer's own prefix
#   - no include directory, library path or compile definition
#
# It also has no access to the source tree, so its <nodehammer/...> includes can
# only resolve to headers that were actually installed.
#
# The one thing it *does* state is the language standard, pinned to the floor the
# installed target promises via INTERFACE_COMPILE_FEATURES. nodehammer is built
# as C++23; its headers must not require it. Compiling at exactly the floor turns
# that from an intention into a check — a std::expected reaching a public header
# fails here rather than in somebody else's C++20 codebase. Keep this in step
# with the INTERFACE floor in CMakeLists.txt; main.cpp avoids <print> for the
# same reason.
cmake_minimum_required(VERSION 3.25)
project(nodehammer_shared_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nodehammer REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE nodehammer::nodehammer)

# Reported, not set. What a consumer is compiled with is exactly what this test
# exists to leave unstated — but when the result misbehaves at run time rather
# than at link time, the build type and the MSVC runtime library are the first
# two things worth knowing, and neither is visible from the failure alone.
message(STATUS "consumer: CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}'")
message(STATUS "consumer: MSVC_RUNTIME_LIBRARY='${CMAKE_MSVC_RUNTIME_LIBRARY}'")
message(STATUS "consumer: CXX_FLAGS='${CMAKE_CXX_FLAGS}'")
