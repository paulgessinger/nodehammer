#include <nodehammer/io.hpp>
#include <nodehammer/nhb.hpp>
#include <nodehammer/nhr.hpp>
// The Python mirror of the public API.
//
// Every declaration here comes from include/nodehammer/. Nothing from src/
// appears, and that is enforced rather than trusted: this module links
// nodehammer_shared, whose internals are hidden, so an internal symbol would
// still *compile* in-tree and then fail to link — the same property
// tests/public/ is built on (tests/CMakeLists.txt).
//
// The mapping rule is: same names, same order, same semantics, snake_case for
// members. Where Python and C++ genuinely disagree the difference is recorded
// at the call site rather than smoothed over, because a binding that quietly
// improves on the API it mirrors is a second API to keep in sync.

#include <nodehammer/build.hpp>
#include <nodehammer/cli.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/version.hpp>

#include <nanobind/make_iterator.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nb = nanobind;
namespace nh = nodehammer;

using namespace nb::literals;

namespace {

/// `formats()` hands back a view over storage the library owns for its own
/// lifetime. Python gets a plain list: a view would be a dangling promise the
/// moment anyone kept it, and the C++ side's pointer-identity guarantee is not
/// something a Python caller can observe or needs.
std::vector<std::string> toStringList(std::span<const std::string_view> formats) {
    return {formats.begin(), formats.end()};
}

/// `std::vector<std::byte>` out of the library, `bytes` into Python. The copy is
/// unavoidable — `bytes` owns its buffer — and these are serialization payloads
/// the caller asked to materialize anyway.
nb::bytes toPyBytes(const std::vector<std::byte> &data) {
    return nb::bytes(reinterpret_cast<const char *>(data.data()), data.size());
}

/// `bytes` into the library. The span is consumed within the call and the
/// argument keeps the buffer alive for its duration, so nothing outlives it.
std::span<const std::byte> asByteSpan(const nb::bytes &data) {
    return {reinterpret_cast<const std::byte *>(data.c_str()), data.size()};
}

/// `base_dir` is None, not "".
///
/// nanobind evaluates a C++ default argument once, at binding time, by casting
/// it to a Python object — and `std::filesystem::path{}` round-trips through
/// pathlib as `PosixPath('.')`, which is a real directory. Defaulting the
/// parameter in C++ would therefore hand the loader a non-empty base and root
/// every include at the process's working directory, silently reinstating the
/// behaviour #52 removed. None carries the meaning the library actually has:
/// this content has no location, so an include resolves to nothing.
std::filesystem::path baseDirOrNone(const std::optional<std::filesystem::path> &baseDir) {
    return baseDir ? *baseDir : std::filesystem::path{};
}

std::string configSource(nb::dict dict) {
    nb::object dumps;
    try {
        dumps = nb::module_::import_("tomli_w").attr("dumps");
    } catch (const nb::python_error &) {
        throw nb::import_error("a dict config needs tomli-w; install nodehammer[dict]");
    }
    return nb::cast<std::string>(dumps(dict));
}

/// The Python `nodehammer.Error`, created once at module init and held for the
/// translator below. `PyErr_NewException` is limited-API, so this survives the
/// stable-ABI build.
nb::object errorType; // NOLINT(cert-err58-cpp) — module-init lifetime, by design

/// docs/error-model.md tier 2: every failure the library can *name* is an
/// `Error`, and everything else propagates unchanged. So this translates
/// `nodehammer::Error` and nothing else — `std::bad_alloc` reaching Python as
/// `MemoryError` is nanobind's default and is the correct outcome, not a gap.
void translateError(const std::exception_ptr &p, void * /*payload*/) {
    try {
        std::rethrow_exception(p);
    } catch (const nh::Error &e) {
        nb::object instance = errorType(e.what());
        instance.attr("code") = e.code();
        instance.attr("context") = e.context();
        instance.attr("diagnostic") = e.diagnostic();

        // `observed()` is a span borrowed from the exception object, which dies
        // when this catch block unwinds. The Python exception outlives it, so
        // this copy is load-bearing rather than defensive.
        const auto observed = e.observed();
        instance.attr("observed") = std::vector<nh::Diagnostic>{observed.begin(), observed.end()};

        PyErr_SetObject(errorType.ptr(), instance.ptr());
    }
}

} // namespace

NB_MODULE(_nodehammer, m) {
    m.doc() = "Detector geometry tessellation and export — the nodehammer public API.";

    // Zero-argument accessors are bound as properties, not methods. Mirroring the
    // C++ API means keeping its names and its semantics, not transliterating its
    // punctuation: Python's way of spelling a cheap side-effect-free read *is* an
    // attribute, so `scene.valid` is the faithful spelling of `scene.valid()`,
    // the same way `format=` is the faithful spelling of ReadOptions{format}.
    //
    // The line is cost, not arity. Anything that serializes (to_nhb), does I/O
    // (read, write) or materializes a container (formats, items) stays a method,
    // so a property never hides work a caller would want to see.

    // ── diagnostics ─────────────────────────────────────────────────────────
    nb::class_<nh::Diagnostic> diagnostic(m, "Diagnostic");

    nb::enum_<nh::Diagnostic::Severity>(diagnostic, "Severity")
        .value("Debug", nh::Diagnostic::Severity::Debug)
        .value("Info", nh::Diagnostic::Severity::Info)
        .value("Warning", nh::Diagnostic::Severity::Warning)
        // `Error` means a partial result, not a failure — the failure channel is
        // the exception. `Fatal` never appears in a list this library returns;
        // it exists for Error::diagnostic(), which is why it is still bound.
        .value("Error", nh::Diagnostic::Severity::Error)
        .value("Fatal", nh::Diagnostic::Severity::Fatal);

    diagnostic.def(nb::init<>())
        .def_rw("severity", &nh::Diagnostic::severity)
        .def_rw("code", &nh::Diagnostic::code)
        .def_rw("message", &nh::Diagnostic::message)
        .def_rw("context", &nh::Diagnostic::context)
        .def("__repr__", [](const nh::Diagnostic &d) {
            std::string out = "<Diagnostic " + d.code + " " + d.message;
            if (!d.context.empty()) {
                out += " (" + d.context + ")";
            }
            return out + ">";
        });

    nb::class_<nh::DiagnosticList>(m, "DiagnosticList")
        .def(nb::init<>())
        .def("add", &nh::DiagnosticList::add, "diagnostic"_a)
        .def("debug", &nh::DiagnosticList::debug, "code"_a, "message"_a, "context"_a = "")
        .def("info", &nh::DiagnosticList::info, "code"_a, "message"_a, "context"_a = "")
        .def("warn", &nh::DiagnosticList::warn, "code"_a, "message"_a, "context"_a = "")
        .def("error", &nh::DiagnosticList::error, "code"_a, "message"_a, "context"_a = "")
        .def("append", &nh::DiagnosticList::append, "other"_a)
        // Answers "is the result partial?", never "did it work" — see
        // docs/error-model.md. A caller that ignores the whole list still holds
        // something valid.
        .def_prop_ro("has_errors", &nh::DiagnosticList::hasErrors)
        .def("items", &nh::DiagnosticList::items)
        .def("__len__", &nh::DiagnosticList::size)
        .def("__bool__", [](const nh::DiagnosticList &l) { return !l.empty(); })
        .def(
            "__iter__",
            [](const nh::DiagnosticList &l) {
                return nb::make_iterator(nb::type<nh::Diagnostic>(), "DiagnosticIterator",
                                         l.begin(), l.end());
            },
            nb::keep_alive<0, 1>());

    errorType = nb::steal(PyErr_NewException("nodehammer.Error", PyExc_RuntimeError, nullptr));
    m.attr("Error") = errorType;
    nb::register_exception_translator(translateError, nullptr);

    // ── config ──────────────────────────────────────────────────────────────
    // Both slices are opaque by design: what they carry is the internal AST,
    // the most volatile struct in the repo (#41 §7). They exist to be passed
    // back into a verb.
    nb::class_<nh::SceneConfig>(m, "SceneConfig")
        .def(nb::init<>())
        .def_prop_ro("valid", &nh::SceneConfig::valid);

    nb::class_<nh::OutputConfig>(m, "OutputConfig")
        .def(nb::init<>())
        .def_prop_ro("valid", &nh::OutputConfig::valid);

    nb::class_<nh::Config>(m, "Config")
        .def(nb::init<>())
        .def_prop_ro("scene", &nh::Config::scene)
        .def_prop_ro("output", &nh::Config::output)
        .def_prop_ro("valid", &nh::Config::valid);
    m.def("read_config", &nh::readConfig, "path"_a, nb::call_guard<nb::gil_scoped_release>());
    m.def(
        "from_toml",
        [](std::string_view toml, const std::optional<std::filesystem::path> &baseDir) {
            nb::gil_scoped_release unlocked;
            return nh::fromToml(toml, baseDirOrNone(baseDir));
        },
        "toml"_a, "base_dir"_a = nb::none());
    m.def(
        "from_dict",
        [](nb::dict dict, const std::optional<std::filesystem::path> &baseDir) {
            const auto source = configSource(dict);
            nb::gil_scoped_release unlocked;
            return nh::fromToml(source, baseDirOrNone(baseDir));
        },
        "config"_a, "base_dir"_a = nb::none());
    m.def("check_config", &nh::checkConfig, "path"_a, nb::call_guard<nb::gil_scoped_release>());
    m.def(
        "check_config_string",
        [](std::string_view toml, const std::optional<std::filesystem::path> &baseDir) {
            nb::gil_scoped_release unlocked;
            return nh::checkConfigString(toml, baseDirOrNone(baseDir));
        },
        "toml"_a, "base_dir"_a = nb::none());
    m.def(
        "check_config_dict",
        [](nb::dict dict, const std::optional<std::filesystem::path> &baseDir) {
            const auto source = configSource(dict);
            nb::gil_scoped_release unlocked;
            return nh::checkConfigString(source, baseDirOrNone(baseDir));
        },
        "config"_a, "base_dir"_a = nb::none());
    m.def("config_formats", [] { return toStringList(nh::configFormats()); });

    nb::class_<nh::ConfigResult>(m, "ConfigResult")
        .def_ro("config", &nh::ConfigResult::config)
        .def_ro("diags", &nh::ConfigResult::diags)
        // `auto [config, diags] = ...` is how every C++ call site reads; this is
        // the same shape in Python.
        .def("__iter__",
             [](const nh::ConfigResult &r) { return nb::iter(nb::make_tuple(r.config, r.diags)); });

    // ── semantic scene ──────────────────────────────────────────────────────
    nb::class_<nh::SemanticScene>(m, "SemanticScene")
        .def(nb::init<>())
        .def_prop_ro("valid", &nh::SemanticScene::valid)
        .def_prop_ro("node_count", &nh::SemanticScene::nodeCount)
        .def_prop_ro("log_vol_count", &nh::SemanticScene::logVolCount)
        .def_prop_ro("shape_count", &nh::SemanticScene::shapeCount)
        .def_prop_ro("material_count", &nh::SemanticScene::materialCount);
    m.def(
        "from_nhb",
        [](const nb::bytes &bytes) {
            const auto span = asByteSpan(bytes);
            nb::gil_scoped_release unlocked;
            return nh::fromNhb(span);
        },
        "bytes"_a);
    m.def(
        "read_semantic",
        [](const std::filesystem::path &path, const std::string &format, const nb::dict &values) {
            nh::ImporterOptions options;
            for (auto [key, value] : values) {
                if (!nb::isinstance<nb::str>(key)) {
                    throw nb::type_error("importer option names must be strings");
                }
                auto name = nb::cast<std::string>(key);
                // Test bool before int: Python bool is an int subclass. No coercion.
                if (nb::isinstance<nb::bool_>(value))
                    options.emplace(name, nb::cast<bool>(value));
                else if (nb::isinstance<nb::int_>(value)) {
                    const auto integer = PyLong_AsLongLong(value.ptr());
                    if (PyErr_Occurred())
                        throw nb::python_error();
                    options.emplace(name, static_cast<std::int64_t>(integer));
                } else if (nb::isinstance<nb::float_>(value))
                    options.emplace(name, nb::cast<double>(value));
                else if (nb::isinstance<nb::str>(value))
                    options.emplace(name, nb::cast<std::string>(value));
                else
                    throw nb::type_error(
                        "importer option values must be bool, int64, float, or str");
            }
            nb::gil_scoped_release unlocked;
            return nh::readSemantic(path,
                                    {.format = format, .importerOptions = std::move(options)});
        },
        "path"_a, "format"_a = "", nb::kw_only(), "importer_options"_a = nb::dict());
    m.def("semantic_read_formats", [] { return toStringList(nh::semanticReadFormats()); });
    m.def("semantic_write_formats", [] { return toStringList(nh::semanticWriteFormats()); });
    m.def(
        "write",
        [](const nh::SemanticScene &scene, const std::filesystem::path &path,
           const std::string &format, int compressionLevel) {
            nb::gil_scoped_release unlocked;
            nh::write(scene, path, {.format = format, .compressionLevel = compressionLevel});
        },
        "scene"_a, "path"_a, "format"_a = "", "compression_level"_a = 3);
    m.def(
        "to_nhb",
        [](const nh::SemanticScene &scene) {
            std::vector<std::byte> bytes;
            {
                nb::gil_scoped_release unlocked;
                bytes = nh::toNhb(scene);
            }
            return toPyBytes(bytes);
        },
        "scene"_a);
    m.def(
        "to_nhb_zstd",
        [](const nh::SemanticScene &scene, int compressionLevel) {
            std::vector<std::byte> bytes;
            {
                nb::gil_scoped_release unlocked;
                bytes = nh::toNhbZstd(scene, compressionLevel);
            }
            return toPyBytes(bytes);
        },
        "scene"_a, "compression_level"_a = 3);

    nb::class_<nh::SemanticResult>(m, "SemanticResult")
        .def_ro("scene", &nh::SemanticResult::scene)
        .def_ro("diags", &nh::SemanticResult::diags)
        .def("__iter__", [](const nh::SemanticResult &r) {
            return nb::iter(nb::make_tuple(r.scene, r.diags));
        });

    // ── render scene ────────────────────────────────────────────────────────
    nb::class_<nh::RenderScene>(m, "RenderScene")
        .def(nb::init<>())
        .def_prop_ro("valid", &nh::RenderScene::valid)
        .def_prop_ro("node_count", &nh::RenderScene::nodeCount)
        .def_prop_ro("mesh_count", &nh::RenderScene::meshCount)
        .def_prop_ro("material_count", &nh::RenderScene::materialCount)
        .def_prop_ro("triangle_count", &nh::RenderScene::triangleCount);
    m.def(
        "from_nhr",
        [](const nb::bytes &bytes) {
            const auto span = asByteSpan(bytes);
            nb::gil_scoped_release unlocked;
            return nh::fromNhr(span);
        },
        "bytes"_a);
    m.def("read_render", &nh::readRender, "path"_a, nb::call_guard<nb::gil_scoped_release>());
    m.def("render_read_formats", [] { return toStringList(nh::renderReadFormats()); });
    m.def("render_write_formats", [] { return toStringList(nh::renderWriteFormats()); });
    m.def(
        "write",
        [](const nh::RenderScene &scene, const std::filesystem::path &path,
           const nh::OutputConfig &output, const std::string &format, int compressionLevel) {
            nb::gil_scoped_release unlocked;
            nh::write(scene, path, output,
                      {.format = format, .compressionLevel = compressionLevel});
        },
        "scene"_a, "path"_a, "output"_a = nh::OutputConfig{}, "format"_a = "",
        "compression_level"_a = 3);
    m.def(
        "to_nhr",
        [](const nh::RenderScene &scene) {
            std::vector<std::byte> bytes;
            {
                nb::gil_scoped_release unlocked;
                bytes = nh::toNhr(scene);
            }
            return toPyBytes(bytes);
        },
        "scene"_a);

    nb::class_<nh::RenderResult>(m, "RenderResult")
        .def_ro("scene", &nh::RenderResult::scene)
        .def_ro("diags", &nh::RenderResult::diags)
        .def("__iter__",
             [](const nh::RenderResult &r) { return nb::iter(nb::make_tuple(r.scene, r.diags)); });

    // ── pipeline verbs ──────────────────────────────────────────────────────
    // All four release the GIL. Measured on ODD (325k nodes, 211k triangles):
    // read 227 ms, build 280 ms, write 631 ms — so `write` is the long pole, not
    // `build`, and none of them is anywhere near the "minutes" an earlier version
    // of this comment claimed. Hundreds of milliseconds is still far too long to
    // hold the interpreter in a notebook or any threaded application, and the
    // boolean/manifold paths are unbounded on hostile geometry in a way a fixed
    // number does not capture. Holding the GIL is what would make this binding
    // worse than a subprocess.
    m.def(
        "apply_selection",
        [](const nh::SemanticScene &scene, const nh::SceneConfig &config) {
            nb::gil_scoped_release unlocked;
            return nh::applySelection(scene, config);
        },
        "scene"_a, "config"_a);

    m.def(
        "deduplicate",
        [](const nh::SemanticScene &scene, const nh::SceneConfig &config) {
            nb::gil_scoped_release unlocked;
            return nh::deduplicate(scene, config);
        },
        "scene"_a, "config"_a);

    m.def(
        "tessellate",
        [](const nh::SemanticScene &scene, const nh::SceneConfig &config) {
            nb::gil_scoped_release unlocked;
            return nh::tessellate(scene, config);
        },
        "scene"_a, "config"_a);

    m.def(
        "build",
        [](const nh::SemanticScene &scene, const nh::SceneConfig &config) {
            nb::gil_scoped_release unlocked;
            return nh::build(scene, config);
        },
        "scene"_a, "config"_a,
        "apply_selection + deduplicate + tessellate, in the order the CLI runs them.");

    // ── the command line ────────────────────────────────────────────────────
    // The same entry point the `nodehammer` executable is a shim over, which is
    // what makes `nodehammer --version` from a wheel and `nodehammer.__version__`
    // the same code rather than two constants a test pins together.
    //
    // Named `cli_run` and wrapped in Python (nodehammer/cli.py): the wrapper is
    // where `args=None` means `sys.argv[1:]`, and keeping it there rather than
    // here avoids reaching into the interpreter's state from C++.
    //
    // Not `_cli_run`, tempting as the underscore is for something meant to be
    // private: nanobind's stub generator omits leading-underscore members, so
    // the .pyi would not carry it while the py.typed beside it promises the
    // package is annotated -- and the wrapper that calls it is the one file a
    // checker would flag. It stays out of `__all__` instead, which is where
    // "private" belongs in Python anyway.
    //
    // It returns the exit code instead of raising, and is the only verb in this
    // module that reports failure that way. That is deliberate -- a command has
    // already printed its diagnosis by the time it answers, so a caller wants
    // the number, not a second telling of it as an exception. `Error` is still
    // raised for a failure that escapes a command body, which is a defect.
    //
    // The GIL goes for the same reason as the verbs above: `run` may spend
    // minutes in a tessellation, and it may also block in a pager waiting for a
    // human to press q.
    //
    // `quiet` is the narration switch, and it defaults on here for the same
    // reason `pager` defaults off: reaching this function means a program
    // called it. The commentary a command writes to stderr -- what it wrote,
    // what it merged -- is for somebody watching, and there is nobody watching.
    // Diagnostics are unaffected; see nodehammer/cli.hpp.
    //
    // `web_assets` is a path the *wrapper* supplies, not the user: it is where
    // the `nodehammer-web` package put the wasm runtime, which C++ cannot find
    // on its own because under a wheel the running executable is the interpreter
    // (see web/runtime_locator.hpp). Empty is the ordinary answer when that
    // package is not installed.
    //
    // Taken as a `std::filesystem::path` so nanobind's caster does the decoding.
    // Through a `std::string` the bytes would be re-decoded in the active code
    // page on Windows, which mangles any path this API cannot assume is ASCII --
    // and a site-packages directory under a user's name is exactly that.
    m.def(
        "cli_run",
        [](const std::vector<std::string> &args, bool pager, bool quiet,
           const std::filesystem::path &webAssets) {
            std::vector<std::string_view> views;
            views.reserve(args.size());
            for (const auto &arg : args) {
                views.emplace_back(arg);
            }
            nh::CliOptions options;
            options.pager = pager;
            options.quiet = quiet;
            options.webAssets = webAssets;

            nb::gil_scoped_release unlocked;
            return nh::runCli(views, options);
        },
        "args"_a, "pager"_a = false, "quiet"_a = true, "web_assets"_a = std::filesystem::path{});

    // ── version ─────────────────────────────────────────────────────────────
    // `version()` is a symbol in the library; VERSION is a constant in the
    // header. A mismatch means the extension found a different libnodehammer
    // than it was built against, which is exactly what the wheel's rpath exists
    // to prevent — so both are exposed and the test suite compares them.
    m.def("version", &nh::version, "The version of the linked library.");
    m.attr("VERSION") = nh::VERSION;
    m.attr("VERSION_MAJOR") = nh::VERSION_MAJOR;
    m.attr("VERSION_MINOR") = nh::VERSION_MINOR;
    m.attr("VERSION_PATCH") = nh::VERSION_PATCH;
}
