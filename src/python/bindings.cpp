#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
// nodehammer::VERSION is a std::string_view; without this caster the module
// initialiser throws std::bad_cast and the import fails.
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <nodehammer/cli/run.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/scene_build.hpp>
#include <nodehammer/version.hpp>

#include <filesystem>
#include <memory>
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

    // ── Real API surface, not just the argv shim ───────────────────────────
    // Bound to answer a specific question: does exposing actual C++ API to
    // Python change what the module exports to the dynamic loader? It does
    // not. These calls are resolved at static link time inside the module,
    // so the export set stays at exactly PyInit__nodehammer no matter how
    // much of nodehammer_lib gets bound here.

    nb::class_<nodehammer::Diagnostic>(m, "Diagnostic")
        .def_ro("code", &nodehammer::Diagnostic::code)
        .def_ro("message", &nodehammer::Diagnostic::message)
        .def_ro("context", &nodehammer::Diagnostic::context)
        .def_prop_ro("severity",
                     [](const nodehammer::Diagnostic &d) {
                         return std::string{nodehammer::severityName(d.severity)};
                     })
        .def_prop_ro("is_fatal", &nodehammer::Diagnostic::isFatal)
        .def("__repr__", [](const nodehammer::Diagnostic &d) {
            return "<Diagnostic " + std::string{nodehammer::severityName(d.severity)} + " " +
                   d.code + ": " + d.message + ">";
        });

    nb::class_<nodehammer::RenderScene>(m, "RenderScene")
        .def_prop_ro("node_count", [](const nodehammer::RenderScene &s) { return s.nodes.size(); })
        .def_prop_ro("mesh_count",
                     [](const nodehammer::RenderScene &s) { return s.meshAssets.size(); })
        .def_prop_ro("material_count",
                     [](const nodehammer::RenderScene &s) { return s.materials.size(); });

    nb::class_<nodehammer::SceneBuildResult>(m, "SceneBuildResult")
        .def_ro("scene", &nodehammer::SceneBuildResult::scene)
        .def_prop_ro("diagnostics", [](const nodehammer::SceneBuildResult &r) {
            // items() hands back the backing vector; nanobind's vector caster
            // turns it into an ordinary Python list of Diagnostic.
            return r.diags.items();
        });

    m.def(
        "build_scene",
        [](const std::filesystem::path &config_path, const std::filesystem::path &geometry_path) {
            nb::gil_scoped_release release;
            return nodehammer::buildSceneFromPaths(config_path, geometry_path);
        },
        nb::arg("config_path"), nb::arg("geometry_path"),
        "Run the conversion pipeline (import -> validate -> select -> dedup -> "
        "tessellate) and return the RenderScene plus diagnostics.");
}
