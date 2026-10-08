#pragma once

// The CLI's internals — what `include/nodehammer/cli.hpp` deliberately does not
// say.
//
// The public header carries one function and a struct of options, because that
// is all an installed consumer can be given: everything else here names
// `CLI::App`, which is CLI11's type rather than ours to publish. Registering a
// subcommand is therefore an in-tree operation, available to whatever links the
// static archive and to nothing else.
//
// The namespace is spelled in the joined form on purpose. `ci/check_shared_exports.py`
// harvests `^namespace nodehammer::x::y` to decide what is internal, and a
// nested `namespace nodehammer::cli { namespace detail {` would register only as
// `nodehammer::cli` — which is *public*, since a public header declares it. The
// joined spelling is what keeps this half hidden in the checker's eyes as well
// as the linker's.

#include <nodehammer/cli.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

// Declared, not included: `CLI::App &` inside a function-pointer type needs no
// definition, so a translation unit that only *passes* a registrar — main.cpp —
// compiles without CLI11's header forest.
namespace nodehammer::ir {
class ImporterRegistry;
}

namespace CLI {
class App;
} // namespace CLI

namespace nodehammer::cli::detail {

/// The exit code, on its way out of a subcommand callback.
///
/// CLI11 callbacks return `void`, so a command that has decided on a non-zero
/// code has no way to hand it back. Throwing is that way: the entry point
/// catches this and returns `code`.
///
/// Deliberately derived from nothing. CLI11 catches `CLI::Error`,
/// `CLI::ValidationError`, `CLI::ConversionError`, `CLI::ArgumentMismatch`,
/// `CLI::FileError` and `std::invalid_argument` at various points inside
/// `App::parse`; a type in none of those hierarchies is the one shape that
/// provably passes through all of them untouched.
///
/// It is not an error type and carries no message — by the time it is thrown the
/// failure has already been reported. `nodehammer::Error` remains the exception
/// that *describes* a failure; this one only carries the number.
struct CommandFailure {
    int code = 1;
};

/// How a subcommand joins the parser.
///
/// `CliOptions` rides along because some commands need to know what kind of
/// caller they have before they do anything — `inspect` asks
/// it whether they may page. It is a reference to the caller's object, which
/// outlives the parse.
using Registrar = void (*)(CLI::App &, const CliOptions &, ir::ImporterRegistry &);

/// `run`, plus subcommands the library cannot register for itself.
///
/// The native viewer brings a window system; the native DD4hep adapter brings
/// output redirection and an exit handler. Both are compiled into
/// the executable and handed in here, outside the callable CLI.
int runWith(std::span<const std::string_view> args, const CliOptions &options,
            std::span<const Registrar> extra);

// The built-in commands. Global scope until now, which is tolerable in an
// executable and not in a shared library: on ELF's flat namespace a bare
// `registerCmdConvert` is a symbol anyone can collide with, and if its
// visibility ever slipped, `ci/check_shared_exports.py` would report it as an
// *unqualified* symbol and point the reader at --exclude-libs, which is not the
// line at fault.
void registerCmdConvert(CLI::App &app, const CliOptions &options, ir::ImporterRegistry &registry);
void registerCmdInspect(CLI::App &app, const CliOptions &options, ir::ImporterRegistry &registry);
void registerCmdConfig(CLI::App &app, const CliOptions &options, ir::ImporterRegistry &registry);

// Native executable only: plugin output redirection and exit warning.
void registerCmdDD4hepNative(CLI::App &app, const CliOptions &options,
                             ir::ImporterRegistry &registry);

// Native-only: packing mounts a `FilesystemProjectFs`, which the web build
// does not have. Registered beside `viewer` for the same reason.
void registerCmdProject(CLI::App &app, const CliOptions &options, ir::ImporterRegistry &registry);

// Native-only for a different reason: every line of it writes to a filesystem
// the user keeps, which under Emscripten is a virtual one that vanishes with the
// tab. The skill bytes themselves are gated with it, so a wasm module carries
// neither the command nor its payload.
void registerCmdSkills(CLI::App &app, const CliOptions &options, ir::ImporterRegistry &registry);

// ── `viewer`, in two halves ───────────────────────────────────────────────────
//
// The library registers the subcommand and serves the browser; the executable
// adds the window's options and takes over the dispatch. They live in different
// binaries — `cmd_viewer_native.cpp` constructs a `viewer::App`, which
// `--no-undefined` on the shared library would turn into a link error rather
// than a missing feature — so nothing can be passed between them at run time.
//
// Nothing needs to be. The parsed values live in the `CLI::App`, so the native
// half reads the shared options straight off the subcommand it was handed, and
// the two halves share declarations rather than state. That is what removes the
// registration singleton this used to need.
//
// None of it is compiled under Emscripten: serving the viewer is something a
// *host* does, and there the module holding this code would be the thing being
// served. `run.cpp` skips the registration to match.

/// Declare the options both modes take.
void addViewerCommonOptions(CLI::App &sub);

/// Declare the options `viewer serve` takes.
void addViewerServeOptions(CLI::App &sub);

/// Stage a root, serve it, open a browser, and wait.
///
/// Takes both Apps because the options are split across them: the shared ones
/// (`path`, `--config`, `--input`, `--title`) live on `viewer`, the server's own
/// on `serve`. Reading them from the parser rather than from a struct is what
/// lets the native half register modes this half declared.
///
/// `options` rides along for `webAssets`: the runtime's location is a property
/// of the front door rather than of the command line, so it arrives with the
/// other front-door properties instead of through an option nobody typed.
void runViewerServe(CLI::App &viewer, CLI::App &serve, const CliOptions &options,
                    ir::ImporterRegistry &registry);

/// Registers `viewer` with the shared and web options, and a callback that
/// serves or explains. The native half replaces that callback.
void registerCmdViewer(CLI::App &app, const CliOptions &options, ir::ImporterRegistry &registry);

/// Extends the `viewer` subcommand with the native window: its options, and the
/// run path plain `viewer` takes.
///
/// Compiled into the executable, never into the library — it constructs a
/// `viewer::App`, which `--no-undefined` on the shared library would turn into a
/// link error rather than a missing feature. Declared here so main.cpp can name
/// it without a second forward declaration going stale.
void registerCmdViewerNative(CLI::App &app, const CliOptions &options,
                             ir::ImporterRegistry &registry);

} // namespace nodehammer::cli::detail
