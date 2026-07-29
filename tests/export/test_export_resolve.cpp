#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nodehammer/export_resolve.hpp>

#include <optional>
#include <string>
#include <utility>

using nodehammer::ExportConfig;
using nodehammer::NHConfig;
using nodehammer::resolveExportConfig;
using Format = nodehammer::ExportConfig::Format;

namespace {

// Every field is optional, mirroring TOML: an omitted key is `std::nullopt`,
// and only a key that is present overrides the format default. Designated
// initializers at the call sites keep partial tables readable.
// The defaults are spelled out so a partial designated initializer does not
// trip -Wmissing-field-initializers under -Wextra.
struct GltfOpts {
    std::optional<double> unitScale = std::nullopt;
    std::optional<bool> bake = std::nullopt;
    std::optional<bool> multiScene = std::nullopt;
    std::optional<std::string> separator = std::nullopt;
};

struct ObjOpts {
    std::optional<double> unitScale = std::nullopt;
    std::optional<bool> bake = std::nullopt;
};

// These return the *variant*, not the concrete format config, and every test
// assigns through them. Assigning a bare `GltfExportFormatConfig` lvalue into
// `exportFormats` instead trips a -Wmaybe-uninitialized false positive on GCC
// 14 and 15 at -O3: `variant::operator=(T&&)` builds an internal temporary, and
// GCC cannot prove the disengaged `optional<std::string>` payload inside it is
// never read. Building the variant here keeps the workaround in one place.
nodehammer::ExportFormatConfig gltfTable(GltfOpts o) {
    nodehammer::GltfExportFormatConfig g;
    g.common.unitScale = o.unitScale;
    g.common.bakeUnitScale = o.bake;
    g.multiScene = o.multiScene;
    g.sceneNameSeparator = std::move(o.separator);
    return g;
}

nodehammer::ExportFormatConfig objTable(ObjOpts o) {
    nodehammer::ObjExportFormatConfig obj;
    obj.common.unitScale = o.unitScale;
    obj.common.bakeUnitScale = o.bake;
    return obj;
}

} // namespace

// ── Format resolution ─────────────────────────────────────────────────────────

TEST_CASE("resolveExportConfig: format comes from the extension", "[export][resolve]") {
    const NHConfig cfg;
    CHECK(resolveExportConfig(cfg, "out.glb").format == Format::GLB);
    CHECK(resolveExportConfig(cfg, "out.gltf").format == Format::GLTF);
    CHECK(resolveExportConfig(cfg, "out.obj").format == Format::OBJ);
}

TEST_CASE("resolveExportConfig: explicit hint overrides the extension", "[export][resolve]") {
    const NHConfig cfg;
    CHECK(resolveExportConfig(cfg, "out.glb", "obj").format == Format::OBJ);
    CHECK(resolveExportConfig(cfg, "out.obj", "gltf").format == Format::GLTF);
}

// ── Format defaults (no config tables) ────────────────────────────────────────

TEST_CASE("resolveExportConfig: empty config yields the format defaults", "[export][resolve]") {
    const NHConfig cfg;

    const auto glb = resolveExportConfig(cfg, "out.glb");
    CHECK(glb.unitScale == Catch::Approx(0.01));
    CHECK_FALSE(glb.bakeUnitScale);
    CHECK_FALSE(glb.gltf.multiScene);
    CHECK(glb.gltf.sceneNameSeparator == " > ");

    // OBJ bakes by default because ObjExporter rejects bakeUnitScale == false.
    const auto obj = resolveExportConfig(cfg, "out.obj");
    CHECK(obj.unitScale == Catch::Approx(0.01));
    CHECK(obj.bakeUnitScale);
}

// ── Per-field overlay ─────────────────────────────────────────────────────────

TEST_CASE("resolveExportConfig: matching table overrides field by field", "[export][resolve]") {
    NHConfig cfg;
    // bake and separator omitted -> their defaults must survive.
    cfg.exportFormats["gltf"] = gltfTable({.unitScale = 0.1, .multiScene = true});

    const auto ecfg = resolveExportConfig(cfg, "out.gltf");
    CHECK(ecfg.unitScale == Catch::Approx(0.1));
    CHECK(ecfg.gltf.multiScene);
    CHECK_FALSE(ecfg.bakeUnitScale);
    CHECK(ecfg.gltf.sceneNameSeparator == " > ");
}

TEST_CASE("resolveExportConfig: obj table applies and can keep the bake default",
          "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["obj"] = objTable({.unitScale = 1.0});

    const auto ecfg = resolveExportConfig(cfg, "out.obj");
    CHECK(ecfg.unitScale == Catch::Approx(1.0));
    CHECK(ecfg.bakeUnitScale); // untouched by the table, so the OBJ default stands
}

TEST_CASE("resolveExportConfig: a non-matching table is ignored", "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["obj"] = objTable({.unitScale = 42.0});

    // Writing glTF must not pick up the OBJ scale.
    CHECK(resolveExportConfig(cfg, "out.gltf").unitScale == Catch::Approx(0.01));
}

// ── GLB → gltf fallback ───────────────────────────────────────────────────────

TEST_CASE("resolveExportConfig: GLB falls back to the gltf table", "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["gltf"] =
        gltfTable({.unitScale = 0.1, .bake = true, .multiScene = true, .separator = "::"});

    const auto ecfg = resolveExportConfig(cfg, "out.glb");
    CHECK(ecfg.format == Format::GLB);
    CHECK(ecfg.unitScale == Catch::Approx(0.1));
    CHECK(ecfg.bakeUnitScale);
    CHECK(ecfg.gltf.multiScene);
    CHECK(ecfg.gltf.sceneNameSeparator == "::");
}

TEST_CASE("resolveExportConfig: an explicit glb table wins over gltf", "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["gltf"] =
        gltfTable({.unitScale = 0.1, .bake = true, .multiScene = true, .separator = "::"});
    cfg.exportFormats["glb"] =
        gltfTable({.unitScale = 0.5, .bake = false, .multiScene = false, .separator = "/"});

    const auto ecfg = resolveExportConfig(cfg, "out.glb");
    CHECK(ecfg.unitScale == Catch::Approx(0.5));
    CHECK_FALSE(ecfg.bakeUnitScale);
    CHECK_FALSE(ecfg.gltf.multiScene);
    CHECK(ecfg.gltf.sceneNameSeparator == "/");
}

// Characterisation, not endorsement: the GLB fallback is table-level, so a
// *present but partially filled* [export.glb] suppresses [export.gltf]
// entirely rather than merging field by field. This is what `convert` has
// always done; the test pins it so the eventual change is a deliberate one.
TEST_CASE("resolveExportConfig: a partial glb table suppresses gltf entirely",
          "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["gltf"] =
        gltfTable({.unitScale = 0.1, .bake = true, .multiScene = true, .separator = "::"});
    cfg.exportFormats["glb"] = gltfTable({.multiScene = false}); // the only field set

    const auto ecfg = resolveExportConfig(cfg, "out.glb");
    CHECK_FALSE(ecfg.gltf.multiScene);
    // gltf's unit_scale / bake / separator are NOT inherited:
    CHECK(ecfg.unitScale == Catch::Approx(0.01));
    CHECK_FALSE(ecfg.bakeUnitScale);
    CHECK(ecfg.gltf.sceneNameSeparator == " > ");
}

// The fallback is GLB-only: writing .gltf never consults [export.glb].
TEST_CASE("resolveExportConfig: gltf output does not fall back to the glb table",
          "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["glb"] =
        gltfTable({.unitScale = 0.5, .bake = true, .multiScene = true, .separator = "/"});

    const auto ecfg = resolveExportConfig(cfg, "out.gltf");
    CHECK(ecfg.unitScale == Catch::Approx(0.01));
    CHECK_FALSE(ecfg.gltf.multiScene);
}

// ── Hint and table interaction ────────────────────────────────────────────────

TEST_CASE("resolveExportConfig: the table is chosen by resolved format, not extension",
          "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["obj"] = objTable({.unitScale = 7.0});

    // Extension says glTF, hint says OBJ -> the OBJ table applies.
    const auto ecfg = resolveExportConfig(cfg, "out.gltf", "obj");
    CHECK(ecfg.format == Format::OBJ);
    CHECK(ecfg.unitScale == Catch::Approx(7.0));
}
