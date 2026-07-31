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
# as C++23; its headers must not require it. Compiling this at exactly the floor
# is what turns that from an intention into a check — a std::span or
# std::expected reaching a public header fails here rather than in somebody
# else's C++17 codebase. Keep this in step with the INTERFACE floor in
# CMakeLists.txt, and note main.cpp deliberately avoids <print> for the same
# reason.
cmake_minimum_required(VERSION 3.25)
project(nodehammer_shared_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(nodehammer REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE nodehammer::nodehammer)
