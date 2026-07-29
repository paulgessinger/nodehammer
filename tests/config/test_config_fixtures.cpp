#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <config/config_loader.hpp>
#include <config/config_validator.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#ifndef NODEHAMMER_FIXTURES_DIR
#error "NODEHAMMER_FIXTURES_DIR must be defined by CMake"
#endif

// Guards against fixture rot: every top-level file under fixtures/configs/ is an
// entry point meant to be loaded standalone (files under includes/ and odd/ are
// fragments, only meaningful when pulled in via `include` and are intentionally
// excluded here). kFixtureCases records the expected load/validate outcome for
// each one; the coverage test below fails if a fixture is added or removed
// without updating the table, so silently-broken or silently-untested fixtures
// can't slip in.

namespace {

const std::filesystem::path fixturesDir{NODEHAMMER_FIXTURES_DIR};

enum class Expect { LoadFails, ValidateFails, Valid };

struct FixtureCase {
    std::string name;
    Expect expect;
};

// Already exercised by dedicated TEST_CASEs in test_config_loader.cpp (which
// assert on parsed content, not just pass/fail) — listed here only so the
// coverage check below doesn't flag them as untracked.
const std::vector<std::string> kCoveredElsewhere = {
    "full_example.toml",    "include_bad_path.toml", "include_basic.toml", "include_cycle.toml",
    "include_diamond.toml", "include_nested.toml",   "minimal.toml",
};

const std::vector<FixtureCase> kFixtureCases = {
    {"invalid_bad_tolerance.toml", Expect::ValidateFails},
    {"invalid_missing_material_ref.toml", Expect::ValidateFails},
    {"odd.toml", Expect::Valid},
    {"odd_drop_coincident_faces.toml", Expect::Valid},
    {"odd_merged.toml", Expect::Valid},
    {"odd_simple.toml", Expect::Valid},
    {"odd_single_ecal_barrel_stave.toml", Expect::Valid},
    {"odd_single_hcal_barrel_stave.toml", Expect::Valid},
    {"odd_single_long_strip_barrel_module.toml", Expect::Valid},
    {"odd_single_long_strip_barrel_stave.toml", Expect::Valid},
    {"odd_single_muon_barrel_chamber.toml", Expect::Valid},
    {"odd_single_pixel_barrel_module.toml", Expect::Valid},
    {"odd_single_pixel_barrel_stave.toml", Expect::Valid},
    {"odd_single_short_strip_barrel_module.toml", Expect::Valid},
    {"odd_single_short_strip_barrel_stave.toml", Expect::Valid},
    {"odd_tracker.toml", Expect::Valid},
};

} // namespace

TEST_CASE("Fixture configs: table covers every top-level *.toml file", "[config][fixtures]") {
    std::set<std::string> onDisk;
    for (const auto &entry : std::filesystem::directory_iterator(fixturesDir / "configs")) {
        if (entry.is_regular_file() && entry.path().extension() == ".toml") {
            onDisk.insert(entry.path().filename().string());
        }
    }

    std::set<std::string> tabulated(kCoveredElsewhere.begin(), kCoveredElsewhere.end());
    for (const auto &c : kFixtureCases) {
        tabulated.insert(c.name);
    }

    REQUIRE(onDisk == tabulated);
}

TEST_CASE("Fixture configs: load/validate outcome matches expectation", "[config][fixtures]") {
    const auto fixture = GENERATE(from_range(kFixtureCases));
    DYNAMIC_SECTION(fixture.name) {
        auto result =
            nodehammer::ConfigLoader::loadFromFile(fixturesDir / "configs" / fixture.name);

        if (fixture.expect == Expect::LoadFails) {
            REQUIRE(result.diags.hasErrors());
            return;
        }
        REQUIRE_FALSE(result.diags.hasErrors());

        auto validationDiags = nodehammer::ConfigValidator::validate(result.config);
        if (fixture.expect == Expect::ValidateFails) {
            REQUIRE(validationDiags.hasErrors());
        } else {
            REQUIRE_FALSE(validationDiags.hasErrors());
        }
    }
}
