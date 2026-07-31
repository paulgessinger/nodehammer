# Stand-in for an external consumer, driven by ci/verify-shared-install.sh. What
# is under test is everything this file does *not* say:
#
#   - no CMAKE_CXX_STANDARD — C++23 arrives via INTERFACE_COMPILE_FEATURES
#   - no find_dependency, no CMAKE_PREFIX_PATH beyond nodehammer's own prefix
#   - no include directory, library path or compile definition
#
# It also has no access to the source tree, so its <nodehammer/...> includes can
# only resolve to headers that were actually installed.

cmake_minimum_required(VERSION 3.25)
project(nodehammer_shared_consumer LANGUAGES CXX)

find_package(nodehammer REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE nodehammer::nodehammer)
