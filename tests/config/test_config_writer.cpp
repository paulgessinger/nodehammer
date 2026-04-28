#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/config_writer.hpp>

using namespace nodehammer;

// ── Helper: round-trip through TOML ─────────────────────────────────────────

static NHConfig roundTrip(const NHConfig &cfg) {
    std::string toml = configToToml(cfg);
    CAPTURE(toml);
    auto result = ConfigLoader::loadFromString(toml, "<roundtrip>");
    REQUIRE_FALSE(result.diags.hasErrors());
    return result.config;
}

// ── Empty config ────────────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: empty config produces valid TOML", "[config][writer]") {
    NHConfig cfg;
    auto toml = configToToml(cfg);
    auto result = ConfigLoader::loadFromString(toml);
    REQUIRE_FALSE(result.diags.hasErrors());
}

// ── Top-level flags ─────────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: top-level flags round-trip", "[config][writer]") {
    NHConfig cfg;
    cfg.hoistOrphans = true;
    cfg.deduplicateShapes = false;
    auto rt = roundTrip(cfg);
    REQUIRE(rt.hoistOrphans == true);
    REQUIRE(rt.deduplicateShapes == false);
}

// ── Materials ───────────────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: material round-trip", "[config][writer]") {
    NHConfig cfg;
    MaterialDef mat;
    mat.name = "steel";
    mat.baseColor = {0.5f, 0.6f, 0.7f, 1.0f};
    mat.metallic = 0.9f;
    mat.roughness = 0.1f;
    mat.doubleSided = false;
    mat.alphaMode = AlphaMode::Mask;
    mat.alphaCutoff = 0.3f;
    mat.ior = 1.5f;
    mat.transmission = 0.2f;
    mat.clearcoat = 0.8f;
    mat.clearcoatRoughness = 0.1f;
    mat.anisotropy = 0.5f;
    mat.anisotropyRotation = 1.0f;
    mat.specularFactor = 0.7f;
    mat.specularColor = Color{0.1f, 0.2f, 0.3f, 1.0f};
    cfg.materials.push_back(mat);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.materials.size() == 1);
    const auto &m = rt.materials[0];
    REQUIRE(m.name == "steel");
    REQUIRE(m.metallic == Catch::Approx(0.9f));
    REQUIRE(m.roughness == Catch::Approx(0.1f));
    REQUIRE(m.doubleSided == false);
    REQUIRE(m.alphaMode == AlphaMode::Mask);
    REQUIRE(m.alphaCutoff == Catch::Approx(0.3f));
    REQUIRE(m.ior.has_value());
    REQUIRE(*m.ior == Catch::Approx(1.5f));
    REQUIRE(m.transmission.has_value());
    REQUIRE(m.clearcoat.has_value());
    REQUIRE(m.clearcoatRoughness.has_value());
    REQUIRE(m.anisotropy.has_value());
    REQUIRE(m.anisotropyRotation.has_value());
    REQUIRE(m.specularFactor.has_value());
    REQUIRE(m.specularColor.has_value());
}

TEST_CASE("ConfigWriter: material with emissive round-trip", "[config][writer]") {
    NHConfig cfg;
    MaterialDef mat;
    mat.name = "glow";
    mat.emissive = {1.0f, 0.5f, 0.0f, 1.0f};
    cfg.materials.push_back(mat);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.materials.size() == 1);
    REQUIRE(rt.materials[0].emissive.r == Catch::Approx(1.0f));
    REQUIRE(rt.materials[0].emissive.g == Catch::Approx(0.5f));
}

// ── Selection rules ─────────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: selection rule with name_glob round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    rule.predicate = PredicateExpr{NameGlobPredicate{"*Tracker*"}};
    rule.scope = "/World/**";
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    REQUIRE(rt.selection[0].action == SelectionAction::KeepIf);
    REQUIRE(rt.selection[0].scope == "/World/**");
    REQUIRE(std::holds_alternative<NameGlobPredicate>(rt.selection[0].predicate.data));
    REQUIRE(std::get<NameGlobPredicate>(rt.selection[0].predicate.data).pattern == "*Tracker*");
}

TEST_CASE("ConfigWriter: selection rule with drop_if round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::DropIf;
    rule.predicate = PredicateExpr{PathGlobPredicate{"/World/Calo/**"}};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    REQUIRE(rt.selection[0].action == SelectionAction::DropIf);
    REQUIRE(std::holds_alternative<PathGlobPredicate>(rt.selection[0].predicate.data));
}

TEST_CASE("ConfigWriter: selection rule with material_glob round-trip", "[config][writer]") {
    // Regression: configToToml emits `type = 'material_glob'` but earlier
    // versions of parsePredicate didn't recognise that type, so a flatten
    // round-trip on any config using material(...) predicates would fail
    // with "unknown predicate type 'material_glob'". This test pins the
    // structured-table form on both sides.
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    rule.predicate = PredicateExpr{MaterialGlobPredicate{"steel*"}};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    REQUIRE(std::holds_alternative<MaterialGlobPredicate>(rt.selection[0].predicate.data));
    REQUIRE(std::get<MaterialGlobPredicate>(rt.selection[0].predicate.data).pattern == "steel*");
}

// ── Rules ───────────────────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: rule with material round-trip", "[config][writer]") {
    NHConfig cfg;
    Rule rule;
    rule.match = PredicateExpr{NameGlobPredicate{"*sensor*"}};
    rule.material = "steel";
    cfg.rules.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.rules.size() == 1);
    REQUIRE(rt.rules[0].material == "steel");
    REQUIRE(rt.rules[0].match.has_value());
}

TEST_CASE("ConfigWriter: rule with tessellation round-trip", "[config][writer]") {
    NHConfig cfg;
    Rule rule;
    Rule::Tessellation tess;
    tess.skipGeometry = true;
    tess.mergeDescendants = false;
    tess.maxSegmentsCircle = 64;
    tess.fallback = BooleanFallback::BBox;
    rule.tessellation = tess;
    cfg.rules.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.rules.size() == 1);
    REQUIRE(rt.rules[0].tessellation.has_value());
    const auto &t = *rt.rules[0].tessellation;
    REQUIRE(t.skipGeometry == true);
    REQUIRE(t.mergeDescendants == false);
    REQUIRE(t.maxSegmentsCircle == 64);
    REQUIRE(t.fallback == BooleanFallback::BBox);
}

TEST_CASE("ConfigWriter: defaults.tessellation round-trip", "[config][writer]") {
    // Regression: configToToml previously emitted nothing for
    // cfg.tessellationDefaults, so config-flatten of a TOML with
    // [defaults.tessellation] produced a file that loaded with the loader's
    // hard-coded defaults — visibly ~3.5x more triangles for the ODD scene
    // because max_segments_circle defaulted instead of being 10.
    NHConfig cfg;
    cfg.tessellationDefaults.maxSegmentsCircle = 10;
    cfg.tessellationDefaults.fallback = BooleanFallback::Skip;

    auto rt = roundTrip(cfg);
    REQUIRE(rt.tessellationDefaults.maxSegmentsCircle == 10);
    REQUIRE(rt.tessellationDefaults.fallback == BooleanFallback::Skip);
}

TEST_CASE("ConfigWriter: defaults.extras round-trip", "[config][writer]") {
    NHConfig cfg;
    cfg.extrasDefaults = nlohmann::json{{"visible", true}, {"opacity", 1.0}};

    auto rt = roundTrip(cfg);
    REQUIRE(rt.extrasDefaults.has_value());
    REQUIRE((*rt.extrasDefaults)["visible"] == true);
    REQUIRE((*rt.extrasDefaults)["opacity"] == 1.0);
}

TEST_CASE("ConfigWriter: rule with extras round-trip", "[config][writer]") {
    NHConfig cfg;
    Rule rule;
    rule.extras = nlohmann::json{{"detector", "pixel"}, {"layer", 3}};
    cfg.rules.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.rules.size() == 1);
    REQUIRE(rt.rules[0].extras.has_value());
    REQUIRE((*rt.rules[0].extras)["detector"] == "pixel");
    REQUIRE((*rt.rules[0].extras)["layer"] == 3);
}

// ── Compound predicates ─────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: and predicate round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    auto andPred = std::make_shared<AndPredicate>(AndPredicate{
        {PredicateExpr{NameGlobPredicate{"*A*"}}, PredicateExpr{PathGlobPredicate{"/X/**"}}}});
    rule.predicate = PredicateExpr{andPred};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    const auto &p = rt.selection[0].predicate;
    REQUIRE(std::holds_alternative<std::shared_ptr<AndPredicate>>(p.data));
    REQUIRE(std::get<std::shared_ptr<AndPredicate>>(p.data)->operands.size() == 2);
}

TEST_CASE("ConfigWriter: not predicate round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::DropIf;
    auto notPred = std::make_shared<NotPredicate>(NotPredicate{PredicateExpr{IsLeafPredicate{}}});
    rule.predicate = PredicateExpr{notPred};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    const auto &p = rt.selection[0].predicate;
    REQUIRE(std::holds_alternative<std::shared_ptr<NotPredicate>>(p.data));
    REQUIRE(std::holds_alternative<IsLeafPredicate>(
        std::get<std::shared_ptr<NotPredicate>>(p.data)->operand.data));
}

// ── Export format config ────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: gltf export config round-trip", "[config][writer]") {
    NHConfig cfg;
    GltfExportFormatConfig gltf;
    gltf.common.unitScale = 0.001;
    gltf.common.bakeUnitScale = true;
    gltf.multiScene = true;
    gltf.sceneNameSeparator = "::";
    cfg.exportFormats["gltf"] = gltf;

    auto rt = roundTrip(cfg);
    REQUIRE(rt.exportFormats.contains("gltf"));
    const auto &v = rt.exportFormats.at("gltf");
    REQUIRE(std::holds_alternative<GltfExportFormatConfig>(v));
    const auto &g = std::get<GltfExportFormatConfig>(v);
    REQUIRE(g.common.unitScale == Catch::Approx(0.001));
    REQUIRE(g.common.bakeUnitScale == true);
    REQUIRE(g.multiScene == true);
    REQUIRE(g.sceneNameSeparator == "::");
}

TEST_CASE("ConfigWriter: obj export config round-trip", "[config][writer]") {
    NHConfig cfg;
    ObjExportFormatConfig obj;
    obj.common.unitScale = 10.0;
    cfg.exportFormats["obj"] = obj;

    auto rt = roundTrip(cfg);
    REQUIRE(rt.exportFormats.contains("obj"));
    const auto &v = rt.exportFormats.at("obj");
    REQUIRE(std::holds_alternative<ObjExportFormatConfig>(v));
    REQUIRE(commonConfig(v).unitScale == Catch::Approx(10.0));
}

// ── Tag predicate ───────────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: tag predicate with value round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    rule.predicate = PredicateExpr{TagPredicate{"semantic", "sensor"}};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    const auto &p = rt.selection[0].predicate;
    REQUIRE(std::holds_alternative<TagPredicate>(p.data));
    const auto &tag = std::get<TagPredicate>(p.data);
    REQUIRE(tag.key == "semantic");
    REQUIRE(tag.value == "sensor");
}

TEST_CASE("ConfigWriter: tag predicate without value round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    rule.predicate = PredicateExpr{TagPredicate{"active", std::nullopt}};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    const auto &tag = std::get<TagPredicate>(rt.selection[0].predicate.data);
    REQUIRE(tag.key == "active");
    REQUIRE_FALSE(tag.value.has_value());
}
