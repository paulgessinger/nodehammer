#include <cli/run_internal.hpp>
#include <nodehammer/dd4hep.hpp>
#include <nodehammer/io.hpp>

#include <DD4hep/Detector.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string_view>

int main(int argc, char **argv) {
    if (argc != 3) {
        return 2;
    }
    try {
        const std::array<std::string_view, 5> args{"inspect", "summary", "-i", argv[1],
                                                   "--dd4hep-global"};
        const std::array extra{static_cast<nodehammer::cli::detail::Registrar>(
            &nodehammer::cli::detail::registerCmdDD4hepNative)};
        if (std::string_view{argv[2]} == "populated") {
            auto &owned = dd4hep::Detector::getInstance();
            owned.fromCompact(argv[1]);
            if (nodehammer::cli::detail::runWith(args, {}, extra) != 1 ||
                nodehammer::fromDD4hep(owned).scene.nodeCount() != 2) {
                throw std::runtime_error("populated detector was not preserved");
            }
        } else if (std::string_view{argv[2]} == "callable") {
            if (nodehammer::runCli(args) != 0 || nodehammer::runCli(args) != 1) {
                throw std::runtime_error("callable CLI global lifecycle failed");
            }
            const std::array<std::string_view, 4> defaults{"inspect", "summary", "-i", argv[1]};
            if (nodehammer::runCli(defaults) != 0) {
                throw std::runtime_error("CLI options leaked across invocations");
            }
        } else {
            if (nodehammer::readSemantic(argv[1]).scene.nodeCount() != 2) {
                throw std::runtime_error("default library import failed");
            }
            // First global import works; a second must not reset the singleton.
            if (nodehammer::readSemantic(argv[1],
                                         {.importerOptions = {{"dd4hep.useGlobalDetector", true}}})
                        .scene.nodeCount() != 2 ||
                nodehammer::cli::detail::runWith(args, {}, extra) != 1) {
                throw std::runtime_error("repeated global import was not rejected");
            }
        }
        // Explicit global options never leak into subsequent default reads.
        if (nodehammer::readSemantic(argv[1]).scene.nodeCount() != 2) {
            throw std::runtime_error("global options leaked into default import");
        }
        // A normal exit after successful/caught imports must not warn.
        std::puts("HOST_RETURNED");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
