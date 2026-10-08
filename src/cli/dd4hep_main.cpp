// A process boundary for DD4hep plugins that require Detector::getInstance().
// Keep this out of runCli: loading such plugins may call exit(), and replacing
// an embedder's default Detector would violate the borrowed-geometry API.
#include "cli_common.hpp"

#include <nodehammer/dd4hep.hpp>
#include <nodehammer/io.hpp>
#include <nodehammer/version.hpp>

#include <DD4hep/Detector.h>
#include <DD4hep/Printout.h>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <print>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

int main(int argc, char **argv) {
    CLI::App app{"Import one DD4hep compact using the process-owned default detector"};
    app.name("nodehammer-dd4hep");
    app.set_version_flag("-V,--version", std::string{nodehammer::VERSION});
    std::string input;
    std::string output;
    app.add_option("-i,--input", input, "DD4hep compact XML file")
        ->required()
        ->check(CLI::ExistingFile);
    app.add_option("-o,--output", output, "Semantic scene (.nhb or .nhb.zst)")->required();
    CLI11_PARSE(app, argc, argv);

    auto suffix = std::filesystem::path{output};
    if (suffix.extension() == ".zst") {
        suffix = suffix.stem();
    }
    if (suffix.extension() != ".nhb") {
        std::println(stderr, "{} output must end in .nhb or .nhb.zst",
                     nodehammer::codes::kFatalCliUsage);
        return 1;
    }

    // Plugins sometimes bypass DD4hep's logging and print directly to stdout.
    // The output is a file, so route those messages to stderr as well. This
    // executable owns the descriptors for its entire remaining lifetime.
    std::fflush(stdout);
#ifdef _WIN32
    const int redirected = _dup2(_fileno(stderr), _fileno(stdout));
#else
    const int redirected = dup2(fileno(stderr), fileno(stdout));
#endif
    if (redirected < 0) {
        std::println(stderr, "{} cannot redirect DD4hep plugin output to stderr",
                     nodehammer::codes::kFatalTgeoOpenFailed);
        return 1;
    }

    try {
        dd4hep::setPrintLevel(dd4hep::ERROR);
        // This is the first and only geometry in a fresh process. The default
        // detector stays alive until exit; no caller-owned instance is touched.
        auto &detector = dd4hep::Detector::getInstance();
        detector.fromCompact(input);
        auto result = nodehammer::fromDD4hep(detector);
        nodehammer::cli::printDiags(result.diags.items());
        if (result.diags.hasErrors()) {
            return 1;
        }
        nodehammer::write(result.scene, output);
        std::println(stderr, "Wrote {} node(s) to {}", result.scene.nodeCount(), output);
        return 0;
    } catch (const nodehammer::Error &error) {
        nodehammer::cli::printDiag(error.diagnostic());
    } catch (const std::exception &error) {
        std::println(stderr, "{} DD4hep import of '{}' failed: {}",
                     nodehammer::codes::kFatalTgeoOpenFailed, input, error.what());
    }
    return 1;
}
