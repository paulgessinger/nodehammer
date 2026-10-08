// Native-executable policy: redirect plugin output and explain premature
// process exit during XML loading. Never linked into the callable CLI or the SDK.
#include "cli_common.hpp"

#include <ir/dd4hep/semantic/importer.hpp>

#include <DD4hep/Detector.h>
#include <DD4hep/Printout.h>
#include <TError.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nodehammer::cli::detail {
namespace {

constinit std::atomic<unsigned> privateImports{0};
constinit std::atomic<unsigned> globalImports{0};
constinit std::atomic_flag importing = ATOMIC_FLAG_INIT;

// exit() skips stack unwinding but runs atexit handlers. Use only trivial
// static state and C stdio here: plugin/ROOT shutdown may already have run.
void warnOnImportExit() {
    if (privateImports.load(std::memory_order_relaxed) != 0) {
        std::fprintf(stderr,
                     "warning %s: process exited during DD4hep XML import. A detector "
                     "plugin may require the global detector (for example ALLEGRO). "
                     "Retry with --dd4hep-global.\n",
                     codes::kWarnDD4hepEarlyExit.data());
        std::fflush(stderr);
    } else if (globalImports.load(std::memory_order_relaxed) != 0) {
        std::fprintf(stderr,
                     "warning %s: process exited during DD4hep XML import while using "
                     "the global detector. A detector plugin may have requested process exit.\n",
                     codes::kWarnDD4hepEarlyExit.data());
        std::fflush(stderr);
    }
}

// A plugin may exit without unwinding. A trivially destructible atomic gate
// avoids leaving a locked static mutex to be destroyed during process exit.
class ImportGate {
  public:
    ImportGate() {
        if (importing.test_and_set(std::memory_order_acquire)) {
            throw Error{codes::kFatalTgeoOpenFailed, "DD4hep XML imports cannot run concurrently"};
        }
    }
    ~ImportGate() { importing.clear(std::memory_order_release); }
    ImportGate(const ImportGate &) = delete;
    ImportGate &operator=(const ImportGate &) = delete;
};

class ImportExitGuard {
  public:
    explicit ImportExitGuard(bool global) : counter_(global ? globalImports : privateImports) {
        counter_.fetch_add(1, std::memory_order_relaxed);
    }
    ~ImportExitGuard() { counter_.fetch_sub(1, std::memory_order_relaxed); }
    ImportExitGuard(const ImportExitGuard &) = delete;
    ImportExitGuard &operator=(const ImportExitGuard &) = delete;

  private:
    std::atomic<unsigned> &counter_;
};

// Plugins sometimes bypass DD4hep logging and write directly to stdout.
// Redirect only while importing, restoring the descriptor even on exceptions.
class ImportLogGuard {
  public:
    ImportLogGuard() {
        std::fflush(stdout);
#ifdef _WIN32
        saved_ = _dup(_fileno(stdout));
        const bool redirected = saved_ >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) >= 0;
#else
        saved_ = dup(fileno(stdout));
        const bool redirected = saved_ >= 0 && dup2(fileno(stderr), fileno(stdout)) >= 0;
#endif
        if (!redirected) {
            closeSaved();
            throw Error{codes::kFatalTgeoOpenFailed, "cannot redirect DD4hep plugin output"};
        }
        dd4hep::setPrintLevel(dd4hep::ERROR);
        gErrorIgnoreLevel = kError;
    }
    ~ImportLogGuard() {
        std::fflush(stdout);
#ifdef _WIN32
        _dup2(saved_, _fileno(stdout));
#else
        dup2(saved_, fileno(stdout));
#endif
        closeSaved();
        dd4hep::setPrintLevel(level_);
        gErrorIgnoreLevel = rootLevel_;
    }
    ImportLogGuard(const ImportLogGuard &) = delete;
    ImportLogGuard &operator=(const ImportLogGuard &) = delete;

  private:
    void closeSaved() {
        if (saved_ >= 0) {
#ifdef _WIN32
            _close(saved_);
#else
            close(saved_);
#endif
        }
    }
    int saved_ = -1;
    dd4hep::PrintLevel level_ = dd4hep::printLevel();
    int rootLevel_ = gErrorIgnoreLevel;
};

class NativeDD4hepImporter final : public ir::ISemanticImporter {
  public:
    explicit NativeDD4hepImporter(std::unique_ptr<ir::ISemanticImporter> importer)
        : importer_(std::move(importer)) {}
    std::span<const ir::ImporterOptionSpec> optionSpecs() const override {
        return importer_->optionSpecs();
    }
    void configure(const ImporterOptions &options) override {
        importer_->configure(options);
        const auto it = options.find("dd4hep.useGlobalDetector");
        global_ = it != options.end() && std::get<bool>(it->second);
    }
    std::string_view formatName() const noexcept override { return "dd4hep"; }
    std::vector<std::string> supportedExtensions() const override { return {}; }
    ir::ImportResult import(const std::filesystem::path &path) const override {
        const ImportGate gate;
        const ImportLogGuard log;
        const ImportExitGuard warning{global_};
        return importer_->import(path);
    }

  private:
    std::unique_ptr<ir::ISemanticImporter> importer_;
    bool global_ = false;
};

} // namespace

void registerCmdDD4hepNative(CLI::App & /*app*/, const CliOptions &,
                             ir::ImporterRegistry &registry) {
    static const int registered = std::atexit(warnOnImportExit);
    if (registered != 0) {
        throw Error{codes::kFatalTgeoOpenFailed, "cannot register DD4hep import exit warning"};
    }
    registry.decorate("dd4hep", [](std::unique_ptr<ir::ISemanticImporter> importer) {
        return std::make_unique<NativeDD4hepImporter>(std::move(importer));
    });
}

} // namespace nodehammer::cli::detail
