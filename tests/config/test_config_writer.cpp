#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <config/config_loader.hpp>
#include <config/config_writer.hpp>

using namespace nodehammer;
using namespace nodehammer::config;

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
    // cfg.tessellationDefaults, so `config flatten` of a TOML with
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
    cfg.extrasDefaults = ExtrasMap::makeObject({{"visible", true}, {"opacity", 1.0}});

    auto rt = roundTrip(cfg);
    REQUIRE(rt.extrasDefaults.has_value());
    REQUIRE(rt.extrasDefaults->at("visible").boolValue() == true);
    REQUIRE(rt.extrasDefaults->at("opacity").doubleValue() == 1.0);
}

TEST_CASE("ConfigWriter: rule with extras round-trip", "[config][writer]") {
    NHConfig cfg;
    Rule rule;
    rule.extras = ExtrasMap::makeObject({{"detector", "pixel"}, {"layer", 3}});
    cfg.rules.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.rules.size() == 1);
    REQUIRE(rt.rules[0].extras.has_value());
    REQUIRE(rt.rules[0].extras->at("detector").stringValue() == "pixel");
    REQUIRE(rt.rules[0].extras->at("layer").intValue() == 3);
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

// ── Constant (bool) predicates ──────────────────────────────────────────────

TEST_CASE("ConfigWriter: always-false predicate round-trip", "[config][writer]") {
    // Regression: configToToml used to encode BoolPredicate{false} as an empty
    // `and` table, which the loader reloads as vacuously true. A flatten
    // round-trip therefore inverted `keep_if = "false"` into "matches
    // everything". Pin the constant on both sides.
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    rule.predicate = PredicateExpr{BoolPredicate{false}};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    const auto &p = rt.selection[0].predicate;
    REQUIRE(std::holds_alternative<BoolPredicate>(p.data));
    REQUIRE(std::get<BoolPredicate>(p.data).value == false);
}

TEST_CASE("ConfigWriter: always-true predicate round-trip", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.action = SelectionAction::KeepIf;
    rule.predicate = PredicateExpr{BoolPredicate{true}};
    cfg.selection.push_back(rule);

    auto rt = roundTrip(cfg);
    REQUIRE(rt.selection.size() == 1);
    const auto &p = rt.selection[0].predicate;
    REQUIRE(std::holds_alternative<BoolPredicate>(p.data));
    REQUIRE(std::get<BoolPredicate>(p.data).value == true);
}

// ── Export format config ────────────────────────────────────────────────────

TEST_CASE("ConfigWriter: gltf export config round-trip", "[config][writer]") {
    NHConfig cfg;
    GltfExportFormatConfig gltf;
    gltf.common.unitScale = 0.001;
    gltf.bakeUnitScale = true;
    gltf.multiScene = true;
    gltf.sceneNameSeparator = "::";
    cfg.exportFormats["gltf"] = gltf;

    auto rt = roundTrip(cfg);
    REQUIRE(rt.exportFormats.contains("gltf"));
    const auto &v = rt.exportFormats.at("gltf");
    REQUIRE(std::holds_alternative<GltfExportFormatConfig>(v));
    const auto &g = std::get<GltfExportFormatConfig>(v);
    REQUIRE(g.common.unitScale == Catch::Approx(0.001));
    REQUIRE(g.bakeUnitScale == true);
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

TEST_CASE("ConfigWriter: logical volume selection roundtrips", "[config][writer]") {
    NHConfig cfg;
    SelectionRule rule;
    rule.predicate = PredicateExpr{VolumeGlobPredicate{"*Toroid*"}};
    cfg.selection.push_back(rule);
    const auto restored = roundTrip(cfg);
    REQUIRE(std::get<VolumeGlobPredicate>(restored.selection.at(0).predicate.data).pattern ==
            "*Toroid*");
}

TEST_CASE("ConfigWriter: geometry replacement round-trips", "[config][writer]") {
    NHConfig cfg;
    Rule rule;
    rule.geometry = Rule::Geometry{"convex_hull", std::nullopt};
    cfg.rules.push_back(rule);
    const auto rt = roundTrip(cfg);
    REQUIRE(rt.rules.size() == 1);
    REQUIRE(rt.rules.front().geometry);
    REQUIRE(rt.rules.front().geometry->replaceDescendants == "convex_hull");
}

TEST_CASE("ConfigWriter: drift chamber predicates round-trip", "[config][writer]") {
    NHConfig cfg;
    Rule rule;
    rule.geometry.emplace();
    auto &drift = rule.geometry->driftChamber.emplace();
    drift.gas = R"(material == "gas")";
    drift.wires = R"(material ~= "*Wire*" && name != "support")";
    cfg.rules.push_back(rule);
    const auto restored = roundTrip(cfg);
    const auto &rt = *restored.rules.at(0).geometry->driftChamber;
    REQUIRE(restored.rules.at(0).geometry->replaceDescendants.empty());
    REQUIRE(rt.detect == drift.detect);
    REQUIRE(rt.gas == drift.gas);
    REQUIRE(rt.wires == drift.wires);
    REQUIRE(rt.gasRepresentation == drift.gasRepresentation);
    REQUIRE(rt.wireRepresentation == drift.wireRepresentation);
}

TEST_CASE("ConfigWriter: wire defaults and explicit zero override round-trip",
          "[config][writer][wire]") {
    NHConfig cfg;
    cfg.tessellationDefaults.wireCut = "centerline";
    cfg.tessellationDefaults.wireLodPixels = 0.75;
    cfg.tessellationDefaults.wireLodDistance = 50;
    auto &rule = cfg.rules.emplace_back();
    rule.tessellation.emplace();
    rule.tessellation->wireCut = "exact";
    rule.tessellation->wireLodPixels = 0;
    rule.tessellation->wireLodDistance = 0;
    const auto rt = roundTrip(cfg);
    CHECK(rt.tessellationDefaults.wireCut == "centerline");
    CHECK(rt.tessellationDefaults.wireLodPixels == 0.75);
    CHECK(rt.tessellationDefaults.wireLodDistance == 50);
    CHECK(rt.rules.front().tessellation->wireCut == "exact");
    CHECK(rt.rules.front().tessellation->wireLodPixels == 0);
    CHECK(rt.rules.front().tessellation->wireLodDistance == 0);
}

TEST_CASE("ConfigWriter: daughter subtraction defaults and override round-trip",
          "[config][writer][subtract_daughters]") {
    NHConfig cfg;
    cfg.tessellationDefaults.subtractDaughters = true;
    cfg.tessellationDefaults.subtractDaughtersMatch = "material == \"foam\"";
    auto &rule = cfg.rules.emplace_back();
    rule.tessellation.emplace();
    rule.tessellation->subtractDaughters = false;
    rule.tessellation->subtractDaughtersMatch = "";
    const auto rt = roundTrip(cfg);
    CHECK(rt.tessellationDefaults.subtractDaughters == true);
    CHECK(rt.tessellationDefaults.subtractDaughtersMatch == "material == \"foam\"");
    CHECK(rt.rules.front().tessellation->subtractDaughters == false);
    CHECK(rt.rules.front().tessellation->subtractDaughtersMatch == "");
}

TEST_CASE("wire policy-only defaults and zero hint survive roundtrip", "[config][wire]") {
    NHConfig cfg;
    cfg.tessellationDefaults.wireLodDistanceFactors = {0.1, 0.4, 2};
    cfg.tessellationDefaults.wireLodFractions = {1, 0.2, 0.05, 0};
    cfg.tessellationDefaults.wireLodHintPixels = 0;
    auto &t = cfg.rules.emplace_back().tessellation.emplace();
    t.wireLodDistanceFactors = {0.2, 0.7, 3};
    t.wireLodFractions = {0.8, 0.4, 0.1, 0.01};
    t.wireLodHintPixels = 1;
    auto rt = roundTrip(cfg);
    CHECK(rt.tessellationDefaults.wireLodDistanceFactors ==
          cfg.tessellationDefaults.wireLodDistanceFactors);
    CHECK(rt.tessellationDefaults.wireLodFractions == cfg.tessellationDefaults.wireLodFractions);
    CHECK(rt.tessellationDefaults.wireLodHintPixels == 0);
    CHECK(rt.rules[0].tessellation->wireLodDistanceFactors == t.wireLodDistanceFactors);
    CHECK(rt.rules[0].tessellation->wireLodFractions == t.wireLodFractions);
    CHECK(rt.rules[0].tessellation->wireLodHintPixels == 1);
}
