#pragma once

#include <CLI/CLI.hpp>
#include <ir/semantic/importer.hpp>
#include <type_traits>

namespace nodehammer::cli::detail {
/// Optional CLI adapter: backend schemas supply names, types, defaults and help.
/// Only this layer depends on CLI11; API calls use the same registry validation.
inline void registerImporterOptions(CLI::App &app, ir::ImporterRegistry &registry) {
    for (const auto &importer : registry.importers()) {
        for (const auto &spec : importer->optionSpecs()) {
            if (spec.cliFlag.empty())
                continue;
            const std::string name{spec.name};
            std::visit(
                [&](const auto &defaultValue) {
                    using T = std::decay_t<decltype(defaultValue)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        app.add_flag_function(
                            std::string{spec.cliFlag},
                            [&registry, name](std::int64_t count) {
                                registry.setOption(name, count > 0);
                            },
                            std::string{spec.description});
                    } else {
                        app.add_option_function<T>(
                            std::string{spec.cliFlag},
                            [&registry, name](const T &value) { registry.setOption(name, value); },
                            std::string{spec.description});
                    }
                },
                spec.defaultValue);
        }
    }
}
} // namespace nodehammer::cli::detail
