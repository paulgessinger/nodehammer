#include <nodehammer/version.hpp>

#include <print>
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
        std::println(stderr, "version mismatch: header says {}, library says {}",
                     nodehammer::VERSION, linked);
        return 1;
    }

    std::println("nodehammer {}", linked);
    return 0;
}
