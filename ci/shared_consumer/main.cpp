#include <nodehammer/version.hpp>

#include <cstdio>
#include <string_view>

// Deliberately links *and runs*: compiling alone proves little, since headers
// install whether or not the library does. version() is out-of-line, so reaching
// it means the export table, the SONAME, and the Windows import library all
// resolved.
int main() {
    const std::string_view linked = nodehammer::version();

    // Header constant vs compiled-in value: a mismatch means headers from one
    // install and a library from another.
    if (linked != nodehammer::VERSION) {
        std::fprintf(stderr, "version mismatch: header says %.*s, library says %.*s\n",
                     static_cast<int>(nodehammer::VERSION.size()), nodehammer::VERSION.data(),
                     static_cast<int>(linked.size()), linked.data());
        return 1;
    }

    std::printf("nodehammer %.*s\n", static_cast<int>(linked.size()), linked.data());
    return 0;
}
