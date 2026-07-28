#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
// nodehammer::VERSION is a std::string_view; without this caster the module
// initialiser throws std::bad_cast and the import fails.
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <nodehammer/cli/run.hpp>
#include <nodehammer/version.hpp>

#include <string>
#include <vector>

namespace nb = nanobind;

namespace {

/// Bridge `sys.argv` into the `main`-shaped signature `nodehammer::cli::run`
/// expects. The strings are owned by `args` for the duration of the call;
/// `std::string::data()` is NUL-terminated since C++11, so the pointers are
/// valid C strings and no copy is needed.
int runMain(std::vector<std::string> args) {
    // CLI11 reads argv[0] for its usage text. A caller passing an empty list
    // (or the bare `main()` default) still deserves sensible `--help` output.
    if (args.empty()) {
        args.emplace_back("nodehammer");
    }

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto &arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr); // main() convention: argv[argc] == nullptr

    // Conversions are long-running and touch no Python state, so let other
    // threads run. This matters more once `serve` lands: a blocking C++ loop
    // holding the GIL would wedge the whole interpreter.
    nb::gil_scoped_release release;
    return nodehammer::cli::run(static_cast<int>(args.size()), argv.data());
}

} // namespace

NB_MODULE(_nodehammer, m) {
    m.doc() = "Native nodehammer core — HEP geometry conversion pipeline.";

    m.def("main", &runMain, nb::arg("argv") = std::vector<std::string>{},
          "Run the nodehammer CLI with the given argv (argv[0] is the program "
          "name). Returns the process exit code.");

    m.attr("__version__") = nodehammer::VERSION;
}
