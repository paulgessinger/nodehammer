#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ir/semantic_json.hpp>

// ── Variant construction ───────────────────────────────────────────────────────
// Each test constructs a SemanticShapeVariant holding the target type.
// This forces complete instantiation of std::variant<...> and would fail to
// compile if any type is incomplete (e.g. forward-declared only).

TEST_CASE("SemanticShapeVariant: holds BoxShape", "[ir][variant]") {
    nodehammer::ir::SemanticShapeVariant v = nodehammer::ir::BoxShape{1.0, 2.0, 3.0};
    REQUIRE(std::holds_alternative<nodehammer::ir::BoxShape>(v));
    REQUIRE(std::get<nodehammer::ir::BoxShape>(v).dx == Catch::Approx(1.0));
}

TEST_CASE("SemanticShapeVariant: holds TubeShape", "[ir][variant]") {
    nodehammer::ir::TubeShape t;
    t.rMin = 5.0;
    t.rMax = 10.0;
    t.dz = 20.0;
    nodehammer::ir::SemanticShapeVariant v = t;
    REQUIRE(std::holds_alternative<nodehammer::ir::TubeShape>(v));
    REQUIRE(std::get<nodehammer::ir::TubeShape>(v).rMax == Catch::Approx(10.0));
}

TEST_CASE("SemanticShapeVariant: holds ConeShape", "[ir][variant]") {
    nodehammer::ir::ConeShape c;
    c.rMin1 = 0.0;
    c.rMax1 = 5.0;
    c.rMin2 = 0.0;
    c.rMax2 = 3.0;
    c.dz = 10.0;
    nodehammer::ir::SemanticShapeVariant v = c;
    REQUIRE(std::holds_alternative<nodehammer::ir::ConeShape>(v));
}

TEST_CASE("SemanticShapeVariant: holds TrdShape", "[ir][variant]") {
    nodehammer::ir::SemanticShapeVariant v = nodehammer::ir::TrdShape{4.0, 2.0, 6.0, 3.0, 5.0};
    REQUIRE(std::holds_alternative<nodehammer::ir::TrdShape>(v));
    REQUIRE(std::get<nodehammer::ir::TrdShape>(v).dx1 == Catch::Approx(4.0));
}

TEST_CASE("SemanticShapeVariant: holds ParaShape", "[ir][variant]") {
    nodehammer::ir::ParaShape p;
    p.dx = 1.0;
    p.dy = 2.0;
    p.dz = 3.0;
    nodehammer::ir::SemanticShapeVariant v = p;
    REQUIRE(std::holds_alternative<nodehammer::ir::ParaShape>(v));
}

TEST_CASE("SemanticShapeVariant: holds PconShape", "[ir][variant]") {
    nodehammer::ir::PconShape pc;
    pc.sections.push_back({0.0, 0.0, 5.0});
    pc.sections.push_back({10.0, 0.0, 5.0});
    nodehammer::ir::SemanticShapeVariant v = pc;
    REQUIRE(std::holds_alternative<nodehammer::ir::PconShape>(v));
    REQUIRE(std::get<nodehammer::ir::PconShape>(v).sections.size() == 2);
}

TEST_CASE("SemanticShapeVariant: holds PgonShape", "[ir][variant]") {
    nodehammer::ir::PgonShape pg;
    pg.nSides = 6;
    pg.sections.push_back({0.0, 0.0, 5.0});
    nodehammer::ir::SemanticShapeVariant v = pg;
    REQUIRE(std::holds_alternative<nodehammer::ir::PgonShape>(v));
    REQUIRE(std::get<nodehammer::ir::PgonShape>(v).nSides == 6);
}

TEST_CASE("SemanticShapeVariant: holds TorusShape", "[ir][variant]") {
    nodehammer::ir::TorusShape t;
    t.rTor = 50.0;
    t.rMax = 5.0;
    nodehammer::ir::SemanticShapeVariant v = t;
    REQUIRE(std::holds_alternative<nodehammer::ir::TorusShape>(v));
    REQUIRE(std::get<nodehammer::ir::TorusShape>(v).rTor == Catch::Approx(50.0));
}

TEST_CASE("SemanticShapeVariant: holds TessellatedShape", "[ir][variant]") {
    nodehammer::ir::TessellatedShape ts;
    ts.triangles.push_back(
        {std::array<glm::dvec3, 3>{glm::dvec3{0, 0, 0}, glm::dvec3{1, 0, 0}, glm::dvec3{0, 1, 0}}});
    nodehammer::ir::SemanticShapeVariant v = ts;
    REQUIRE(std::holds_alternative<nodehammer::ir::TessellatedShape>(v));
    REQUIRE(std::get<nodehammer::ir::TessellatedShape>(v).triangles.size() == 1);
}

TEST_CASE("SemanticShapeVariant: holds UnknownShape", "[ir][variant]") {
    nodehammer::ir::SemanticShapeVariant v = nodehammer::ir::UnknownShape{"TGeoArb8"};
    REQUIRE(std::holds_alternative<nodehammer::ir::UnknownShape>(v));
    REQUIRE(std::get<nodehammer::ir::UnknownShape>(v).originalType == "TGeoArb8");
}

TEST_CASE("SemanticShapeVariant: holds BooleanUnion", "[ir][variant]") {
    nodehammer::ir::BooleanUnion bu;
    bu.left = nodehammer::ir::SemanticShapeId{1};
    bu.right = nodehammer::ir::SemanticShapeId{2};
    nodehammer::ir::SemanticShapeVariant v = bu;
    REQUIRE(std::holds_alternative<nodehammer::ir::BooleanUnion>(v));
    REQUIRE(std::get<nodehammer::ir::BooleanUnion>(v).left.value == 1);
    REQUIRE(std::get<nodehammer::ir::BooleanUnion>(v).right.value == 2);
}

TEST_CASE("SemanticShapeVariant: holds BooleanIntersection", "[ir][variant]") {
    nodehammer::ir::BooleanIntersection bi;
    bi.left = nodehammer::ir::SemanticShapeId{3};
    bi.right = nodehammer::ir::SemanticShapeId{4};
    nodehammer::ir::SemanticShapeVariant v = bi;
    REQUIRE(std::holds_alternative<nodehammer::ir::BooleanIntersection>(v));
}

TEST_CASE("SemanticShapeVariant: holds BooleanSubtraction", "[ir][variant]") {
    nodehammer::ir::BooleanSubtraction bs;
    bs.left = nodehammer::ir::SemanticShapeId{5};
    bs.right = nodehammer::ir::SemanticShapeId{6};
    nodehammer::ir::SemanticShapeVariant v = bs;
    REQUIRE(std::holds_alternative<nodehammer::ir::BooleanSubtraction>(v));
    REQUIRE(std::get<nodehammer::ir::BooleanSubtraction>(v).left.value == 5);
}

// ── std::visit exercises all arms ─────────────────────────────────────────────

TEST_CASE("SemanticShapeVariant: std::visit dispatches to_json for each type", "[ir][variant]") {
    using V = nodehammer::ir::SemanticShapeVariant;
    auto checkType = [](const V &v, std::string_view expectedType) {
        nlohmann::json j;
        std::visit([&j](const auto &s) { to_json(j, s); }, v);
        REQUIRE(j["type"].get<std::string>() == expectedType);
    };

    checkType(nodehammer::ir::BoxShape{1, 2, 3}, "box");
    checkType(nodehammer::ir::TubeShape{}, "tube");
    checkType(nodehammer::ir::ConeShape{}, "cone");
    checkType(nodehammer::ir::TrdShape{}, "trd");
    checkType(nodehammer::ir::ParaShape{}, "para");
    checkType(nodehammer::ir::PconShape{}, "pcon");
    checkType(nodehammer::ir::PgonShape{}, "pgon");
    checkType(nodehammer::ir::TorusShape{}, "torus");
    checkType(nodehammer::ir::TessellatedShape{}, "tessellated");
    checkType(nodehammer::ir::UnknownShape{"Foo"}, "unknown");
    checkType(nodehammer::ir::BooleanUnion{}, "union");
    checkType(nodehammer::ir::BooleanIntersection{}, "intersection");
    checkType(nodehammer::ir::BooleanSubtraction{}, "subtraction");
}

// ── isBooleanShape ────────────────────────────────────────────────────────────

TEST_CASE("isBooleanShape: true only for the three CSG variants", "[ir][variant]") {
    using namespace nodehammer;
    // Boolean variants.
    REQUIRE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::BooleanUnion{}}));
    REQUIRE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::BooleanIntersection{}}));
    REQUIRE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::BooleanSubtraction{}}));
    // A representative selection of primitive / non-boolean variants.
    REQUIRE_FALSE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::BoxShape{1, 2, 3}}));
    REQUIRE_FALSE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::TubeShape{}}));
    REQUIRE_FALSE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::TessellatedShape{}}));
    REQUIRE_FALSE(ir::isBooleanShape(ir::SemanticShapeVariant{ir::UnknownShape{"Foo"}}));
}

TEST_CASE("is_boolean_shape_v: compile-time trait matches the runtime check", "[ir][variant]") {
    using namespace nodehammer;
    STATIC_REQUIRE(ir::is_boolean_shape_v<ir::BooleanUnion>);
    STATIC_REQUIRE(ir::is_boolean_shape_v<ir::BooleanIntersection>);
    STATIC_REQUIRE(ir::is_boolean_shape_v<ir::BooleanSubtraction>);
    STATIC_REQUIRE_FALSE(ir::is_boolean_shape_v<ir::BoxShape>);
    STATIC_REQUIRE_FALSE(ir::is_boolean_shape_v<ir::TorusShape>);
    STATIC_REQUIRE_FALSE(ir::is_boolean_shape_v<ir::UnknownShape>);
}

// ── Default phiDelta uses std::numbers::pi ────────────────────────────────────

TEST_CASE("TubeShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::TubeShape t;
    REQUIRE(t.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("ConeShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::ConeShape c;
    REQUIRE(c.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("PconShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::PconShape p;
    REQUIRE(p.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("PgonShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::PgonShape p;
    REQUIRE(p.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("TorusShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::TorusShape t;
    REQUIRE(t.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}
