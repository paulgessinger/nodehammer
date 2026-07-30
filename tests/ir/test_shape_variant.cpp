#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ir/semantic_json.hpp>

// ── Variant construction ───────────────────────────────────────────────────────
// Each test constructs a semantic::ShapeVariant holding the target type.
// This forces complete instantiation of std::variant<...> and would fail to
// compile if any type is incomplete (e.g. forward-declared only).

TEST_CASE("semantic::ShapeVariant: holds BoxShape", "[ir][variant]") {
    nodehammer::ir::semantic::ShapeVariant v = nodehammer::ir::semantic::BoxShape{1.0, 2.0, 3.0};
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::BoxShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::BoxShape>(v).dx == Catch::Approx(1.0));
}

TEST_CASE("semantic::ShapeVariant: holds TubeShape", "[ir][variant]") {
    nodehammer::ir::semantic::TubeShape t;
    t.rMin = 5.0;
    t.rMax = 10.0;
    t.dz = 20.0;
    nodehammer::ir::semantic::ShapeVariant v = t;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::TubeShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::TubeShape>(v).rMax == Catch::Approx(10.0));
}

TEST_CASE("semantic::ShapeVariant: holds ConeShape", "[ir][variant]") {
    nodehammer::ir::semantic::ConeShape c;
    c.rMin1 = 0.0;
    c.rMax1 = 5.0;
    c.rMin2 = 0.0;
    c.rMax2 = 3.0;
    c.dz = 10.0;
    nodehammer::ir::semantic::ShapeVariant v = c;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::ConeShape>(v));
}

TEST_CASE("semantic::ShapeVariant: holds TrdShape", "[ir][variant]") {
    nodehammer::ir::semantic::ShapeVariant v =
        nodehammer::ir::semantic::TrdShape{4.0, 2.0, 6.0, 3.0, 5.0};
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::TrdShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::TrdShape>(v).dx1 == Catch::Approx(4.0));
}

TEST_CASE("semantic::ShapeVariant: holds ParaShape", "[ir][variant]") {
    nodehammer::ir::semantic::ParaShape p;
    p.dx = 1.0;
    p.dy = 2.0;
    p.dz = 3.0;
    nodehammer::ir::semantic::ShapeVariant v = p;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::ParaShape>(v));
}

TEST_CASE("semantic::ShapeVariant: holds PconShape", "[ir][variant]") {
    nodehammer::ir::semantic::PconShape pc;
    pc.sections.push_back({0.0, 0.0, 5.0});
    pc.sections.push_back({10.0, 0.0, 5.0});
    nodehammer::ir::semantic::ShapeVariant v = pc;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::PconShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::PconShape>(v).sections.size() == 2);
}

TEST_CASE("semantic::ShapeVariant: holds PgonShape", "[ir][variant]") {
    nodehammer::ir::semantic::PgonShape pg;
    pg.nSides = 6;
    pg.sections.push_back({0.0, 0.0, 5.0});
    nodehammer::ir::semantic::ShapeVariant v = pg;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::PgonShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::PgonShape>(v).nSides == 6);
}

TEST_CASE("semantic::ShapeVariant: holds TorusShape", "[ir][variant]") {
    nodehammer::ir::semantic::TorusShape t;
    t.rTor = 50.0;
    t.rMax = 5.0;
    nodehammer::ir::semantic::ShapeVariant v = t;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::TorusShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::TorusShape>(v).rTor == Catch::Approx(50.0));
}

TEST_CASE("semantic::ShapeVariant: holds TessellatedShape", "[ir][variant]") {
    nodehammer::ir::semantic::TessellatedShape ts;
    ts.triangles.push_back(
        {std::array<glm::dvec3, 3>{glm::dvec3{0, 0, 0}, glm::dvec3{1, 0, 0}, glm::dvec3{0, 1, 0}}});
    nodehammer::ir::semantic::ShapeVariant v = ts;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::TessellatedShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::TessellatedShape>(v).triangles.size() == 1);
}

TEST_CASE("semantic::ShapeVariant: holds UnknownShape", "[ir][variant]") {
    nodehammer::ir::semantic::ShapeVariant v = nodehammer::ir::semantic::UnknownShape{"TGeoArb8"};
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::UnknownShape>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::UnknownShape>(v).originalType == "TGeoArb8");
}

TEST_CASE("semantic::ShapeVariant: holds BooleanUnion", "[ir][variant]") {
    nodehammer::ir::semantic::BooleanUnion bu;
    bu.left = nodehammer::ir::semantic::ShapeId{1};
    bu.right = nodehammer::ir::semantic::ShapeId{2};
    nodehammer::ir::semantic::ShapeVariant v = bu;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::BooleanUnion>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::BooleanUnion>(v).left.value == 1);
    REQUIRE(std::get<nodehammer::ir::semantic::BooleanUnion>(v).right.value == 2);
}

TEST_CASE("semantic::ShapeVariant: holds BooleanIntersection", "[ir][variant]") {
    nodehammer::ir::semantic::BooleanIntersection bi;
    bi.left = nodehammer::ir::semantic::ShapeId{3};
    bi.right = nodehammer::ir::semantic::ShapeId{4};
    nodehammer::ir::semantic::ShapeVariant v = bi;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::BooleanIntersection>(v));
}

TEST_CASE("semantic::ShapeVariant: holds BooleanSubtraction", "[ir][variant]") {
    nodehammer::ir::semantic::BooleanSubtraction bs;
    bs.left = nodehammer::ir::semantic::ShapeId{5};
    bs.right = nodehammer::ir::semantic::ShapeId{6};
    nodehammer::ir::semantic::ShapeVariant v = bs;
    REQUIRE(std::holds_alternative<nodehammer::ir::semantic::BooleanSubtraction>(v));
    REQUIRE(std::get<nodehammer::ir::semantic::BooleanSubtraction>(v).left.value == 5);
}

// ── std::visit exercises all arms ─────────────────────────────────────────────

TEST_CASE("semantic::ShapeVariant: std::visit dispatches to_json for each type", "[ir][variant]") {
    using V = nodehammer::ir::semantic::ShapeVariant;
    auto checkType = [](const V &v, std::string_view expectedType) {
        nlohmann::json j;
        std::visit([&j](const auto &s) { to_json(j, s); }, v);
        REQUIRE(j["type"].get<std::string>() == expectedType);
    };

    checkType(nodehammer::ir::semantic::BoxShape{1, 2, 3}, "box");
    checkType(nodehammer::ir::semantic::TubeShape{}, "tube");
    checkType(nodehammer::ir::semantic::ConeShape{}, "cone");
    checkType(nodehammer::ir::semantic::TrdShape{}, "trd");
    checkType(nodehammer::ir::semantic::ParaShape{}, "para");
    checkType(nodehammer::ir::semantic::PconShape{}, "pcon");
    checkType(nodehammer::ir::semantic::PgonShape{}, "pgon");
    checkType(nodehammer::ir::semantic::TorusShape{}, "torus");
    checkType(nodehammer::ir::semantic::TessellatedShape{}, "tessellated");
    checkType(nodehammer::ir::semantic::UnknownShape{"Foo"}, "unknown");
    checkType(nodehammer::ir::semantic::BooleanUnion{}, "union");
    checkType(nodehammer::ir::semantic::BooleanIntersection{}, "intersection");
    checkType(nodehammer::ir::semantic::BooleanSubtraction{}, "subtraction");
}

// ── isBooleanShape ────────────────────────────────────────────────────────────

TEST_CASE("isBooleanShape: true only for the three CSG variants", "[ir][variant]") {
    using namespace nodehammer;
    // Boolean variants.
    REQUIRE(ir::semantic::isBooleanShape(ir::semantic::ShapeVariant{ir::semantic::BooleanUnion{}}));
    REQUIRE(ir::semantic::isBooleanShape(
        ir::semantic::ShapeVariant{ir::semantic::BooleanIntersection{}}));
    REQUIRE(ir::semantic::isBooleanShape(
        ir::semantic::ShapeVariant{ir::semantic::BooleanSubtraction{}}));
    // A representative selection of primitive / non-boolean variants.
    REQUIRE_FALSE(
        ir::semantic::isBooleanShape(ir::semantic::ShapeVariant{ir::semantic::BoxShape{1, 2, 3}}));
    REQUIRE_FALSE(
        ir::semantic::isBooleanShape(ir::semantic::ShapeVariant{ir::semantic::TubeShape{}}));
    REQUIRE_FALSE(
        ir::semantic::isBooleanShape(ir::semantic::ShapeVariant{ir::semantic::TessellatedShape{}}));
    REQUIRE_FALSE(ir::semantic::isBooleanShape(
        ir::semantic::ShapeVariant{ir::semantic::UnknownShape{"Foo"}}));
}

TEST_CASE("is_boolean_shape_v: compile-time trait matches the runtime check", "[ir][variant]") {
    using namespace nodehammer;
    STATIC_REQUIRE(ir::semantic::is_boolean_shape_v<ir::semantic::BooleanUnion>);
    STATIC_REQUIRE(ir::semantic::is_boolean_shape_v<ir::semantic::BooleanIntersection>);
    STATIC_REQUIRE(ir::semantic::is_boolean_shape_v<ir::semantic::BooleanSubtraction>);
    STATIC_REQUIRE_FALSE(ir::semantic::is_boolean_shape_v<ir::semantic::BoxShape>);
    STATIC_REQUIRE_FALSE(ir::semantic::is_boolean_shape_v<ir::semantic::TorusShape>);
    STATIC_REQUIRE_FALSE(ir::semantic::is_boolean_shape_v<ir::semantic::UnknownShape>);
}

// ── Default phiDelta uses std::numbers::pi ────────────────────────────────────

TEST_CASE("TubeShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::semantic::TubeShape t;
    REQUIRE(t.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("ConeShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::semantic::ConeShape c;
    REQUIRE(c.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("PconShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::semantic::PconShape p;
    REQUIRE(p.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("PgonShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::semantic::PgonShape p;
    REQUIRE(p.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}

TEST_CASE("TorusShape: default phiDelta is 2*pi", "[ir][variant]") {
    nodehammer::ir::semantic::TorusShape t;
    REQUIRE(t.phiDelta == Catch::Approx(2.0 * std::numbers::pi));
}
