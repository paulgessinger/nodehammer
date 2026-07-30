# Stand-in for an external consumer of the installed shared library, driven by
# ci/shared-build.sh. Kept deliberately minimal, because what is under test is
# everything this file does *not* have to say:
#
#   - no CMAKE_CXX_STANDARD. The C++23 requirement arrives through the exported
#     target's INTERFACE_COMPILE_FEATURES. If that is missing the build fails
#     here rather than in a consumer's repository.
#   - no find_dependency / CMAKE_PREFIX_PATH for zstd, flatbuffers, manifold,
#     toml++ or the rest. The shared library absorbed them (#41 §10); the only
#     prefix this project is given is nodehammer's own.
#   - no include directory, no library path, no compile definition.
#
# It also has no access to the source tree, so an internal header that leaked
# into the install is the only way its <nodehammer/...> includes could resolve
# to anything but the public set.

cmake_minimum_required(VERSION 3.25)
project(nodehammer_shared_consumer LANGUAGES CXX)

find_package(nodehammer REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE nodehammer::nodehammer)
