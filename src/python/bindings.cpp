#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
// nodehammer::VERSION is a std::string_view; without this caster the module
// initialiser throws std::bad_cast and the import fails.
#include <nanobind/ndarray.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <nodehammer/cli/run.hpp>
#include <nodehammer/ir/diagnostics.hpp>
#include <nodehammer/pipeline.hpp>
#include <nodehammer/render.hpp>
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

    // Mesh buffers are handed to numpy as views over the scene's own bytes —
    // no copy, no serialization round-trip. The `owner` argument is what makes
    // that safe: it ties the array's lifetime to this Mesh object, which owns a
    // refcount on the scene. Taking `nb::object self` rather than a reference is
    // load-bearing for exactly that reason.
    //
    // The strides encode Vertex's layout (24 bytes, normal at offset 12), which
    // schemas/render.fbs already pins as a public contract.
    using Mesh = nodehammer::MeshView;
    constexpr std::size_t kVertexStride = sizeof(nodehammer::Vertex) / sizeof(float);

    auto vertexAttr = [](nb::object self, std::size_t offset) {
        const auto &mesh = nb::cast<const Mesh &>(self);
        const auto verts = mesh.vertices();
        const float *base =
            verts.empty() ? nullptr : reinterpret_cast<const float *>(verts.data()) + offset;
        return nb::ndarray<nb::numpy, const float, nb::shape<-1, 3>>(base, {verts.size(), 3}, self,
                                                                     {kVertexStride, 1});
    };

    nb::class_<Mesh>(m, "Mesh")
        .def_prop_ro("id", [](const Mesh &mv) { return mv.id().value; })
        .def_prop_ro("name", [](const Mesh &mv) { return std::string{mv.name()}; })
        .def_prop_ro("vertex_count", &Mesh::vertexCount)
        .def_prop_ro("index_count", &Mesh::indexCount)
        .def_prop_ro("triangle_count", &Mesh::triangleCount)
        .def_prop_ro("positions",
                     [vertexAttr](nb::object self) { return vertexAttr(std::move(self), 0); })
        .def_prop_ro("normals",
                     [vertexAttr](nb::object self) { return vertexAttr(std::move(self), 3); })
        .def_prop_ro("indices",
                     [](nb::object self) {
                         const auto &mesh = nb::cast<const Mesh &>(self);
                         const auto idx = mesh.indices();
                         return nb::ndarray<nb::numpy, const std::uint32_t, nb::shape<-1, 3>>(
                             idx.empty() ? nullptr : idx.data(), {idx.size() / 3, 3}, self, {3, 1});
                     })
        .def("__repr__", [](const Mesh &mv) {
            return "<Mesh " + std::string{mv.name()} +
                   " tris=" + std::to_string(mv.triangleCount()) + ">";
        });

    nb::class_<nodehammer::RenderScene>(m, "RenderScene")
        .def_prop_ro("valid", &nodehammer::RenderScene::valid)
        .def_prop_ro("node_count", &nodehammer::RenderScene::nodeCount)
        .def_prop_ro("mesh_count", &nodehammer::RenderScene::meshCount)
        .def_prop_ro("material_count", &nodehammer::RenderScene::materialCount)
        .def_prop_ro("triangle_count", &nodehammer::RenderScene::triangleCount)
        .def_prop_ro("mesh_ids",
                     [](const nodehammer::RenderScene &s) {
                         std::vector<std::uint64_t> ids;
                         ids.reserve(s.meshIds().size());
                         for (const auto id : s.meshIds()) {
                             ids.push_back(id.value);
                         }
                         return ids;
                     })
        .def(
            "mesh",
            [](const nodehammer::RenderScene &s, std::uint64_t id) {
                return s.mesh(nodehammer::MeshAssetId{id});
            },
            nb::arg("mesh_id"), "Look up a mesh by id; returns None if absent.")
        .def("__bool__", &nodehammer::RenderScene::valid)
        .def("__repr__", [](const nodehammer::RenderScene &s) {
            if (!s.valid()) {
                return std::string{"<RenderScene invalid>"};
            }
            return "<RenderScene nodes=" + std::to_string(s.nodeCount()) +
                   " meshes=" + std::to_string(s.meshCount()) +
                   " tris=" + std::to_string(s.triangleCount()) + ">";
        });

    nb::class_<nodehammer::BuildResult>(m, "BuildResult")
        // Returned by value: the handle is cheap to copy and owns its own
        // refcount, so the scene outlives this result without a keep_alive.
        .def_prop_ro("scene", [](const nodehammer::BuildResult &r) { return r.scene; })
        .def_prop_ro("diagnostics", [](const nodehammer::BuildResult &r) {
            // items() hands back the backing vector; nanobind's vector caster
            // turns it into an ordinary Python list of Diagnostic.
            return r.diags.items();
        });

    m.def(
        "build_scene",
        [](const std::filesystem::path &config_path, const std::filesystem::path &geometry_path) {
            nb::gil_scoped_release release;
            return nodehammer::buildScene(config_path, geometry_path);
        },
        nb::arg("config_path"), nb::arg("geometry_path"),
        "Run the conversion pipeline (import -> validate -> select -> dedup -> "
        "tessellate) and return the RenderScene plus diagnostics.");
}
