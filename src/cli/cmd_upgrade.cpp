#include "cli_common.hpp"
#include "run_internal.hpp"
#include <CLI/CLI.hpp>
#include <filesystem>
#include <ir/semantic/flatbuffer.hpp>
#include <print>

namespace nodehammer::cli::detail {
void registerCmdUpgrade(CLI::App &app, const CliOptions &options) {
    const Narrator say{options};
    auto *sub = app.add_subcommand("upgrade", "Upgrade an NHB geometry to the current format");
    auto *input = sub->add_option("-i,--input", "Existing NHS8 or NHS9 geometry")->required();
    auto *output = sub->add_option("-o,--output", "Output .nhb or .nhb.zst file")->required();
    sub->callback([=] {
        runOrReport("upgrade", [&] {
            std::string in, out;
            input->results(in);
            output->results(out);
            try {
                if (std::filesystem::weakly_canonical(in) ==
                        std::filesystem::weakly_canonical(out) ||
                    (std::filesystem::exists(out) && std::filesystem::equivalent(in, out))) {
                    throw Error{
                        codes::kFatalCliUsage,
                        "upgrade output must differ from input; the original file is preserved"};
                }
                auto geometry = ir::semantic::readFlatbuffer(in);
                ir::semantic::writeFlatbuffer(geometry, out);
                say("Upgraded {} to NHS9: {} occurrences", in, geometry.validate());
                std::println("{}", out);
            } catch (const Error &) {
                throw;
            } catch (const std::exception &e) {
                throw Error{codes::kFatalExportWriteFailed, e.what(), out};
            }
        });
    });
}
} // namespace nodehammer::cli::detail
