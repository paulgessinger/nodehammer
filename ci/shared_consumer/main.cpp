#include <nodehammer/version.hpp>

#include <print>
#include <string_view>

// Links the installed shared library and calls the one public entity that
// exists so far. Compiling alone would not prove much — headers install
// whether or not the library does — so the check is deliberately a *link* and
// a *run*: version() is out-of-line, so reaching it means the export table,
// the SONAME and the import library (on Windows) all resolved.
int main() {
    const std::string_view linked = nodehammer::version();

    // VERSION is baked into the header at configure time; version() is compiled
    // into the library. A mismatch here means the consumer picked up headers
    // from one install and a library from another — the failure mode the
    // out-of-line accessor exists to make visible.
    if (linked != nodehammer::VERSION) {
        std::println(stderr, "version mismatch: header says {}, library says {}", nodehammer::VERSION,
                     linked);
        return 1;
    }

    std::println("nodehammer {}", linked);
    return 0;
}
