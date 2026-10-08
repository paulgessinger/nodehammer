#include <cli/importer_options.hpp>
#include <ir/semantic/importer.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {
namespace nh = nodehammer;
class ConfigurableImporter final : public nh::ir::ISemanticImporter {
  public:
    std::string_view formatName() const noexcept override { return "probe"; }
    std::vector<std::string> supportedExtensions() const override { return {"probe"}; }
    nh::ir::ImportResult import(const std::filesystem::path &) const override { return {}; }
    std::span<const nh::ir::ImporterOptionSpec> optionSpecs() const override {
        static const nh::ir::ImporterOptionSpec specs[]{
            {"probe.enabled", false, "--probe-enabled", "Enable probe"},
            {"probe.count", std::int64_t{3}, "--probe-count", "Count"},
            {"probe.scale", 1.5, "--probe-scale", "Scale"},
            {"probe.label", std::string{"default"}, "--probe-label", "Label"},
        };
        return specs;
    }
    void configure(const nh::ImporterOptions &options) override { received = options; }
    nh::ImporterOptions received;
};
} // namespace

TEST_CASE("Importer schemas validate primitive types and restore defaults", "[import-options]") {
    nh::ir::ImporterRegistry registry;
    auto owned = std::make_unique<ConfigurableImporter>();
    auto &probe = *owned;
    registry.registerImporter(std::move(owned));
    const nh::ImporterOptions options{{"probe.enabled", true},
                                      {"probe.count", std::int64_t{-5}},
                                      {"probe.scale", 2.5},
                                      {"probe.label", "custom"}};
    REQUIRE(std::holds_alternative<std::string>(options.at("probe.label")));
    registry.configure(options);
    REQUIRE(probe.received == options);
    REQUIRE(registry.resolve("input.probe") == &probe);
    for (const auto &invalid :
         std::vector<nh::ImporterOptions>{{{"probe.typo", true}},
                                          {{"probe.enabled", std::int64_t{1}}},
                                          {{"probe.count", 1.0}},
                                          {{"probe.scale", std::int64_t{1}}},
                                          {{"probe.label", true}}}) {
        REQUIRE_THROWS_AS(registry.configure(invalid), nh::Error);
        REQUIRE(probe.received == options);
    }
    registry.configure({});
    REQUIRE(std::get<bool>(probe.received.at("probe.enabled")) == false);
    REQUIRE(std::get<std::int64_t>(probe.received.at("probe.count")) == 3);
    REQUIRE(std::get<double>(probe.received.at("probe.scale")) == 1.5);
    REQUIRE(std::get<std::string>(probe.received.at("probe.label")) == "default");
}

TEST_CASE("Importer CLI options are generated from the primitive schema", "[cli][import-options]") {
    nh::ir::ImporterRegistry registry;
    auto owned = std::make_unique<ConfigurableImporter>();
    auto &probe = *owned;
    registry.registerImporter(std::move(owned));
    registry.configure({});
    CLI::App app;
    nh::cli::detail::registerImporterOptions(app, registry);
    const char *args[]{"probe", "--probe-enabled", "--probe-count", "-7", "--probe-scale",
                       "2.5",   "--probe-label",   "custom"};
    app.parse(8, args);
    REQUIRE(probe.received == nh::ImporterOptions{{"probe.enabled", true},
                                                  {"probe.count", std::int64_t{-7}},
                                                  {"probe.scale", 2.5},
                                                  {"probe.label", "custom"}});
}
