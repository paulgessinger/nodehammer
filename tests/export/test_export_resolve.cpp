#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nodehammer/export_resolve.hpp>

using nodehammer::ExportConfig;
using nodehammer::NHConfig;
using nodehammer::resolveExportConfig;
using Format = nodehammer::ExportConfig::Format;

namespace {

/// `[export.gltf]` with every field set, so a test can tell which table won.
nodehammer::GltfExportFormatConfig gltfTable(double unitScale, bool bake, bool multiScene,
                                             std::string separator) {
    nodehammer::GltfExportFormatConfig g;
    g.common.unitScale = unitScale;
    g.common.bakeUnitScale = bake;
    g.multiScene = multiScene;
    g.sceneNameSeparator = std::move(separator);
    return g;
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
    nodehammer::GltfExportFormatConfig g;
    g.common.unitScale = 0.1; // set
    g.multiScene = true;      // set
    // bakeUnitScale and sceneNameSeparator left unset -> defaults survive.
    cfg.exportFormats["gltf"] = g;

    const auto ecfg = resolveExportConfig(cfg, "out.gltf");
    CHECK(ecfg.unitScale == Catch::Approx(0.1));
    CHECK(ecfg.gltf.multiScene);
    CHECK_FALSE(ecfg.bakeUnitScale);
    CHECK(ecfg.gltf.sceneNameSeparator == " > ");
}

TEST_CASE("resolveExportConfig: obj table applies and can keep the bake default",
          "[export][resolve]") {
    NHConfig cfg;
    nodehammer::ObjExportFormatConfig o;
    o.common.unitScale = 1.0;
    cfg.exportFormats["obj"] = o;

    const auto ecfg = resolveExportConfig(cfg, "out.obj");
    CHECK(ecfg.unitScale == Catch::Approx(1.0));
    CHECK(ecfg.bakeUnitScale); // untouched by the table, so the OBJ default stands
}

TEST_CASE("resolveExportConfig: a non-matching table is ignored", "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["obj"] = [] {
        nodehammer::ObjExportFormatConfig o;
        o.common.unitScale = 42.0;
        return o;
    }();

    // Writing glTF must not pick up the OBJ scale.
    CHECK(resolveExportConfig(cfg, "out.gltf").unitScale == Catch::Approx(0.01));
}

// ── GLB → gltf fallback ───────────────────────────────────────────────────────

TEST_CASE("resolveExportConfig: GLB falls back to the gltf table", "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["gltf"] = gltfTable(0.1, true, true, "::");

    const auto ecfg = resolveExportConfig(cfg, "out.glb");
    CHECK(ecfg.format == Format::GLB);
    CHECK(ecfg.unitScale == Catch::Approx(0.1));
    CHECK(ecfg.bakeUnitScale);
    CHECK(ecfg.gltf.multiScene);
    CHECK(ecfg.gltf.sceneNameSeparator == "::");
}

TEST_CASE("resolveExportConfig: an explicit glb table wins over gltf", "[export][resolve]") {
    NHConfig cfg;
    cfg.exportFormats["gltf"] = gltfTable(0.1, true, true, "::");
    cfg.exportFormats["glb"] = gltfTable(0.5, false, false, "/");

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
    cfg.exportFormats["gltf"] = gltfTable(0.1, true, true, "::");

    nodehammer::GltfExportFormatConfig glbOnly;
    glbOnly.multiScene = false; // the only field set
    cfg.exportFormats["glb"] = glbOnly;

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
    cfg.exportFormats["glb"] = gltfTable(0.5, true, true, "/");

    const auto ecfg = resolveExportConfig(cfg, "out.gltf");
    CHECK(ecfg.unitScale == Catch::Approx(0.01));
    CHECK_FALSE(ecfg.gltf.multiScene);
}

// ── Hint and table interaction ────────────────────────────────────────────────

TEST_CASE("resolveExportConfig: the table is chosen by resolved format, not extension",
          "[export][resolve]") {
    NHConfig cfg;
    nodehammer::ObjExportFormatConfig o;
    o.common.unitScale = 7.0;
    cfg.exportFormats["obj"] = o;

    // Extension says glTF, hint says OBJ -> the OBJ table applies.
    const auto ecfg = resolveExportConfig(cfg, "out.gltf", "obj");
    CHECK(ecfg.format == Format::OBJ);
    CHECK(ecfg.unitScale == Catch::Approx(7.0));
}
