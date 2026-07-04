#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nodehammer/ir/semantic_json.hpp>

// ── Variant construction ───────────────────────────────────────────────────────
// Each test constructs a SemanticShapeVariant holding the target type.
// This forces complete instantiation of std::variant<...> and would fail to
// compile if any type is incomplete (e.g. forward-declared only).

TEST_CASE("SemanticShapeVariant: holds BoxShape", "[ir][variant]") {
    nodehammer::SemanticShapeVariant v = nodehammer::BoxShape{1.0, 2.0, 3.0};
    REQUIRE(std::holds_alternative<nodehammer::BoxShape>(v));
    REQUIRE(std::get<nodehammer::BoxShape>(v).dx == Catch::Approx(1.0));
}

TEST_CASE("SemanticShapeVariant: holds TubeShape", "[ir][variant]") {
    nodehammer::TubeShape t;
    t.rMin = 5.0;
    t.rMax = 10.0;
    t.dz = 20.0;
    nodehammer::SemanticShapeVariant v = t;
    REQUIRE(std::holds_alternative<nodehammer::TubeShape>(v));
    REQUIRE(std::get<nodehammer::TubeShape>(v).rMax == Catch::Approx(10.0));
}

TEST_CASE("SemanticShapeVariant: holds ConeShape", "[ir][variant]") {
    nodehammer::ConeShape c;
    c.rMin1 = 0.0;
    c.rMax1 = 5.0;
    c.rMin2 = 0.0;
    c.rMax2 = 3.0;
    c.dz = 10.0;
    nodehammer::SemanticShapeVariant v = c;
    REQUIRE(std::holds_alternative<nodehammer::ConeShape>(v));
}

TEST_CASE("SemanticShapeVariant: holds TrdShape", "[ir][variant]") {
    nodehammer::SemanticShapeVariant v = nodehammer::TrdShape{4.0, 2.0, 6.0, 3.0, 5.0};
    REQUIRE(std::holds_alternative<nodehammer::TrdShape>(v));
    REQUIRE(std::get<nodehammer::TrdShape>(v).dx1 == Catch::Approx(4.0));
}

TEST_CASE("SemanticShapeVariant: holds ParaShape", "[ir][variant]") {
    nodehammer::ParaShape p;
    p.dx = 1.0;
    p.dy = 2.0;
    p.dz = 3.0;
    nodehammer::SemanticShapeVariant v = p;
    REQUIRE(std::holds_alternative<nodehammer::ParaShape>(v));
}

TEST_CASE("SemanticShapeVariant: holds PconShape", "[ir][variant]") {
    nodehammer::PconShape pc;
    pc.sections.push_back({0.0, 0.0, 5.0});
    pc.sections.push_back({10.0, 0.0, 5.0});
    nodehammer::SemanticShapeVariant v = pc;
    REQUIRE(std::holds_alternative<nodehammer::PconShape>(v));
    REQUIRE(std::get<nodehammer::PconShape>(v).sections.size() == 2);
}

TEST_CASE("SemanticShapeVariant: holds PgonShape", "[ir][variant]") {
    nodehammer::PgonShape pg;
    pg.nSides = 6;
    pg.sections.push_back({0.0, 0.0, 5.0});
    nodehammer::SemanticShapeVariant v = pg;
    REQUIRE(std::holds_alternative<nodehammer::PgonShape>(v));
    REQUIRE(std::get<nodehammer::PgonShape>(v).nSides == 6);
}

TEST_CASE("SemanticShapeVariant: holds TorusShape", "[ir][variant]") {
    nodehammer::TorusShape t;
    t.rTor = 50.0;
    t.rMax = 5.0;
    nodehammer::SemanticShapeVariant v = t;
    REQUIRE(std::holds_alternative<nodehammer::TorusShape>(v));
    REQUIRE(std::get<nodehammer::TorusShape>(v).rTor == Catch::Approx(50.0));
}

TEST_CASE("SemanticShapeVariant: holds TessellatedShape", "[ir][variant]") {
    nodehammer::TessellatedShape ts;
    ts.triangles.push_back(
        {std::array<glm::dvec3, 3>{glm::dvec3{0, 0, 0}, glm::dvec3{1, 0, 0}, glm::dvec3{0, 1, 0}}});
    nodehammer::SemanticShapeVariant v = ts;
    REQUIRE(std::holds_alternative<nodehammer::TessellatedShape>(v));
    REQUIRE(std::get<nodehammer::TessellatedShape>(v).triangles.size() == 1);
}

TEST_CASE("SemanticShapeVariant: holds UnknownShape", "[ir][variant]") {
    nodehammer::SemanticShapeVariant v = nodehammer::UnknownShape{"TGeoArb8"};
    REQUIRE(std::holds_alternative<nodehammer::UnknownShape>(v));
    REQUIRE(std::get<nodehammer::UnknownShape>(v).originalType == "TGeoArb8");
}

TEST_CASE("SemanticShapeVariant: holds BooleanUnion", "[ir][variant]") {
    nodehammer::BooleanUnion bu;
    bu.left = nodehammer::SemanticShapeId{1};
    bu.right = nodehammer::SemanticShapeId{2};
    nodehammer::SemanticShapeVariant v = bu;
    REQUIRE(std::holds_alternative<nodehammer::BooleanUnion>(v));
    REQUIRE(std::get<nodehammer::BooleanUnion>(v).left.value == 1);
    REQUIRE(std::get<nodehammer::BooleanUnion>(v).right.value == 2);
}

TEST_CASE("SemanticShapeVariant: holds BooleanIntersection", "[ir][variant]") {
    nodehammer::BooleanIntersection bi;
    bi.left = nodehammer::SemanticShapeId{3};
    bi.right = nodehammer::SemanticShapeId{4};
    nodehammer::SemanticShapeVariant v = bi;
    REQUIRE(std::holds_alternative<nodehammer::BooleanIntersection>(v));
}

TEST_CASE("SemanticShapeVariant: holds BooleanSubtraction", "[ir][variant]") {
    nodehammer::BooleanSubtraction bs;
    bs.left = nodehammer::SemanticShapeId{5};
    bs.right = nodehammer::SemanticShapeId{6};
    nodehammer::SemanticShapeVariant v = bs;
    REQUIRE(std::holds_alternative<nodehammer::BooleanSubtraction>(v));
    REQUIRE(std::get<nodehammer::BooleanSubtraction>(v).left.value == 5);
}

// ── std::visit exercises all arms ─────────────────────────────────────────────

TEST_CASE("SemanticShapeVariant: std::visit dispatches to_json for each type", "[ir][variant]") {
    using V = nodehammer::SemanticShapeVariant;
    auto checkType = [](const V &v, std::string_view expectedType) {
        nlohmann::json j;
        std::visit([&j](const auto &s) { to_json(j, s); }, v);
        REQUIRE(j["type"].get<std::string>() == expectedType);
    };

    checkType(nodehammer::BoxShape{1, 2, 3}, "box");
    checkType(nodehammer::TubeShape{}, "tube");
    checkType(nodehammer::ConeShape{}, "cone");
    checkType(nodehammer::TrdShape{}, "trd");
    checkType(nodehammer::ParaShape{}, "para");
    checkType(nodehammer::PconShape{}, "pcon");
    checkType(nodehammer::PgonShape{}, "pgon");
    checkType(nodehammer::TorusShape{}, "torus");
    checkType(nodehammer::TessellatedShape{}, "tessellated");
    checkType(nodehammer::UnknownShape{"Foo"}, "unknown");
    checkType(nodehammer::BooleanUnion{}, "union");
    checkType(nodehammer::BooleanIntersection{}, "intersection");
    checkType(nodehammer::BooleanSubtraction{}, "subtraction");
}

// ── isBooleanShape ────────────────────────────────────────────────────────────

TEST_CASE("isBooleanShape: true only for the three CSG variants", "[ir][variant]") {
    using namespace nodehammer;
    // Boolean variants.
    REQUIRE(isBooleanShape(SemanticShapeVariant{BooleanUnion{}}));
    REQUIRE(isBooleanShape(SemanticShapeVariant{BooleanIntersection{}}));
    REQUIRE(isBooleanShape(SemanticShapeVariant{BooleanSubtraction{}}));
    // A representative selection of primitive / non-boolean variants.
    REQUIRE_FALSE(isBooleanShape(SemanticShapeVariant{BoxShape{1, 2, 3}}));
    REQUIRE_FALSE(isBooleanShape(SemanticShapeVariant{TubeShape{}}));
    REQUIRE_FALSE(isBooleanShape(SemanticShapeVariant{TessellatedShape{}}));
    REQUIRE_FALSE(isBooleanShape(SemanticShapeVariant{UnknownShape{"Foo"}}));
}

TEST_CASE("is_boolean_shape_v: compile-time trait matches the runtime check", "[ir][variant]") {
    using namespace nodehammer;
    STATIC_REQUIRE(is_boolean_shape_v<BooleanUnion>);
    STATIC_REQUIRE(is_boolean_shape_v<BooleanIntersection>);
    STATIC_REQUIRE(is_boolean_shape_v<BooleanSubtraction>);
    STATIC_REQUIRE_FALSE(is_boolean_shape_v<BoxShape>);
    STATIC_REQUIRE_FALSE(is_boolean_shape_v<TorusShape>);
    STATIC_REQUIRE_FALSE(is_boolean_shape_v<UnknownShape>);
}

// ── Default phiDelta uses std::numbers::pi ────────────────────────────────────

TEST_CASE("TubeShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::TubeShape t;
    REQUIRE(t.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("ConeShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ConeShape c;
    REQUIRE(c.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("PconShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::PconShape p;
    REQUIRE(p.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("PgonShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::PgonShape p;
    REQUIRE(p.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("TorusShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::TorusShape t;
    REQUIRE(t.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}
