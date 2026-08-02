// The two reporting channels, seen from outside the library.
//
// `DiagnosticList` is entirely inline, so these cases compile it in *this*
// translation unit and then hand the result across the boundary — which is the
// only way to notice if the two sides disagreed about its layout.
// `Error` is the opposite: every member is out of line and exported, and a
// thrown one has to arrive with its type identity intact.
//
// NH codes are spelled as literals here on purpose. `src/diagnostic_codes.hpp`
// is internal, so a consumer matching on a code has only the published string —
// and these cases assert the strings are what the documentation says, which is
// the whole meaning of calling a code stable.

#include "public_fixture.hpp"

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <span>
#include <string>
#include <vector>

namespace nh = nodehammer;

TEST_CASE("DiagnosticList records at each severity it offers", "[public][diagnostics]") {
    nh::DiagnosticList diags;
    REQUIRE(diags.empty());
    REQUIRE(diags.size() == 0);
    REQUIRE_FALSE(diags.hasErrors());

    diags.debug("NH0508", "trace");
    diags.info("NH0200", "merged 3 shapes");
    diags.warn("NH0005", "unknown key", "cfg.toml");
    REQUIRE(diags.size() == 3);
    REQUIRE_FALSE(diags.hasErrors());

    diags.error("NH0500", "no tessellator", "/world");
    REQUIRE(diags.hasErrors());
    REQUIRE(diags.size() == 4);

    const auto &items = diags.items();
    REQUIRE(items.size() == 4);
    REQUIRE(items[0].severity == nh::Diagnostic::Severity::Debug);
    REQUIRE(items[1].severity == nh::Diagnostic::Severity::Info);
    REQUIRE(items[2].severity == nh::Diagnostic::Severity::Warning);
    REQUIRE(items[3].severity == nh::Diagnostic::Severity::Error);
    REQUIRE(items[2].context == "cfg.toml");
    REQUIRE(items[3].code == "NH0500");

    // `add` takes a whole Diagnostic, which is the only way a caller can
    // construct one at a severity the emitters do not offer.
    diags.add(nh::Diagnostic{nh::Diagnostic::Severity::Info, "NH0000", "by hand", ""});
    REQUIRE(diags.size() == 5);
}

TEST_CASE("DiagnosticList iterates through its pointer pair", "[public][diagnostics]") {
    nh::DiagnosticList diags;
    REQUIRE(diags.begin() == diags.end());

    diags.info("NH0200", "a");
    diags.warn("NH0005", "b");

    std::vector<std::string> messages;
    for (const auto &d : diags) {
        messages.push_back(d.message);
    }
    REQUIRE(messages == std::vector<std::string>{"a", "b"});
}

TEST_CASE("DiagnosticList append and take", "[public][diagnostics]") {
    nh::DiagnosticList first;
    first.info("NH0200", "first");

    nh::DiagnosticList second;
    second.error("NH0500", "second");

    first.append(second);
    REQUIRE(first.size() == 2);
    REQUIRE(first.hasErrors());
    REQUIRE(second.size() == 1); // append copies; the source is untouched

    const std::vector<nh::Diagnostic> taken = std::move(first).take();
    REQUIRE(taken.size() == 2);
    REQUIRE(taken[1].message == "second");
}

TEST_CASE("hasErrors over a borrowed range", "[public][diagnostics]") {
    REQUIRE_FALSE(nh::hasErrors(std::span<const nh::Diagnostic>{}));

    const std::vector<nh::Diagnostic> warned{
        {nh::Diagnostic::Severity::Warning, "NH0005", "unknown key", ""}};
    REQUIRE_FALSE(nh::hasErrors(warned));

    const std::vector<nh::Diagnostic> failed{
        {nh::Diagnostic::Severity::Error, "NH0500", "no mesh", ""}};
    REQUIRE(nh::hasErrors(failed));

    // Fatal is above Error on the ladder, so it counts too — the comparison is
    // `>=`, not `==`.
    const std::vector<nh::Diagnostic> fatal{
        {nh::Diagnostic::Severity::Fatal, "NH0100", "no such file", ""}};
    REQUIRE(nh::hasErrors(fatal));
}

TEST_CASE("Error carries a code, a context and a message", "[public][diagnostics][error]") {
    const nh::Error plain{"NH0000", "something went wrong"};
    REQUIRE(plain.code() == "NH0000");
    REQUIRE(plain.context().empty());
    REQUIRE(std::string{plain.what()} == "something went wrong");
    REQUIRE(plain.observed().empty());

    const nh::Error located{"NH0000", "something went wrong", "/world/inner"};
    REQUIRE(located.context() == "/world/inner");

    // The message is `what()`, so a caller who knows nothing about this library
    // still gets something useful out of `catch (const std::exception &)`.
    const std::exception &asStd = plain;
    REQUIRE(std::string{asStd.what()} == "something went wrong");
}

TEST_CASE("Error::diagnostic is the only Fatal in the system", "[public][diagnostics][error]") {
    const nh::Error err{"NH0100", "no such file", "/nope.nhb"};
    const nh::Diagnostic d = err.diagnostic();
    REQUIRE(d.severity == nh::Diagnostic::Severity::Fatal);
    REQUIRE(d.code == "NH0100");
    REQUIRE(d.message == "no such file");
    REQUIRE(d.context == "/nope.nhb");
}

TEST_CASE("Error carries what was observed before it", "[public][diagnostics][error]") {
    nh::DiagnosticList observed;
    observed.warn("NH0005", "unknown key");
    observed.error("NH0002", "undefined material");

    const nh::Error err{"NH0002", "config did not validate", "cfg.toml", observed};
    REQUIRE(err.observed().size() == 2);
    REQUIRE(err.observed()[0].code == "NH0005");
    REQUIRE(nh::hasErrors(err.observed()));

    // Copying the exception shares the list rather than duplicating it; both
    // views have to stay valid either way.
    const nh::Error copy = err;
    REQUIRE(copy.observed().size() == 2);
    REQUIRE(copy.observed().data() == err.observed().data());
}

TEST_CASE("an Error thrown by the library keeps its type across the boundary",
          "[public][diagnostics][error]") {
    // The one case that needs NH_API_TYPE on the class: matching `catch (const
    // Error &)` against something thrown inside the shared object requires both
    // sides to agree on the type's identity, which on ELF means its typeinfo
    // has to be exported.
    bool caught = false;
    try {
        (void)nh::SemanticScene::read("/nodehammer/definitely/not/here.nhb");
    } catch (const nh::Error &e) {
        caught = true;
        REQUIRE(e.code() == "NH0100");
        REQUIRE(e.diagnostic().severity == nh::Diagnostic::Severity::Fatal);
    }
    REQUIRE(caught);
}

TEST_CASE("no DiagnosticList the library returns carries Fatal", "[public][diagnostics]") {
    // Invariant 1 of docs/error-model.md, checked over every list a caller can
    // get hold of. Structurally guaranteed — `DiagnosticList` has no `fatal()` —
    // but `add` takes a whole Diagnostic, so the guarantee is about what the
    // library does rather than what the type permits.
    const auto imported = nh::SemanticScene::read("", nh::SemanticScene::ReadOptions{"synthetic"});
    REQUIRE_FALSE(nhtest::anyFatal(imported.diags));

    const auto config = nh::Config::parse("deduplicate_shapes = true\n");
    REQUIRE_FALSE(nhtest::anyFatal(config.diags));
    REQUIRE_FALSE(nhtest::anyFatal(nh::Config::checkString("[[rules]]\nmatch = \"!!!\"\n")));

    const auto scene = config.config.scene();
    REQUIRE_FALSE(nhtest::anyFatal(nh::applySelection(imported.scene, scene).diags));
    REQUIRE_FALSE(nhtest::anyFatal(nh::deduplicate(imported.scene, scene).diags));
    REQUIRE_FALSE(nhtest::anyFatal(nh::tessellate(imported.scene, scene).diags));
    REQUIRE_FALSE(nhtest::anyFatal(nh::build(imported.scene, scene).diags));
}
