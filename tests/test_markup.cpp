#include <cstdio>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <detail/markup.hpp>

using nodehammer::detail::ColorMode;
using nodehammer::detail::Console;
using nodehammer::detail::markup;
using nodehammer::detail::markupFormat;
using nodehammer::detail::stripMarkup;

TEST_CASE("markup: plain text passes through unchanged", "[markup]") {
    CHECK(markup("hello world") == "hello world");
    CHECK(markup("") == "");
    CHECK(markup("no tags here 123") == "no tags here 123");
}

TEST_CASE("markup: basic color tags", "[markup]") {
    CHECK(markup("[red]error[/red]") == "\033[31merror\033[39m");
    CHECK(markup("[green]ok[/green]") == "\033[32mok\033[39m");
    CHECK(markup("[blue]info[/blue]") == "\033[34minfo\033[39m");
}

TEST_CASE("markup: bright color tags", "[markup]") {
    CHECK(markup("[bright_red]error[/bright_red]") == "\033[91merror\033[39m");
    CHECK(markup("[bright_cyan]info[/bright_cyan]") == "\033[96minfo\033[39m");
}

TEST_CASE("markup: style tags", "[markup]") {
    CHECK(markup("[bold]important[/bold]") == "\033[1mimportant\033[22m");
    CHECK(markup("[dim]muted[/dim]") == "\033[2mmuted\033[22m");
    CHECK(markup("[italic]emphasis[/italic]") == "\033[3memphasis\033[23m");
    CHECK(markup("[underline]link[/underline]") == "\033[4mlink\033[24m");
    CHECK(markup("[strikethrough]old[/strikethrough]") == "\033[9mold\033[29m");
}

TEST_CASE("markup: reset-all tag", "[markup]") {
    CHECK(markup("[bold][red]text[/]") == "\033[1m\033[31mtext\033[0m");
}

TEST_CASE("markup: nested tags", "[markup]") {
    auto result = markup("[bold][red]error[/red][/bold]");
    CHECK(result == "\033[1m\033[31merror\033[39m\033[22m");
}

TEST_CASE("markup: unknown tags are left as-is", "[markup]") {
    CHECK(markup("[unknown]text[/unknown]") == "[unknown]text[/unknown]");
    CHECK(markup("[foo]bar") == "[foo]bar");
}

TEST_CASE("markup: escaped brackets", "[markup]") {
    CHECK(markup("\\[red]not a tag") == "[red]not a tag");
    CHECK(markup("a\\[b") == "a[b");
}

TEST_CASE("markup: unclosed bracket is literal", "[markup]") {
    CHECK(markup("text [with no close") == "text [with no close");
}

TEST_CASE("stripMarkup: removes tags", "[markup]") {
    CHECK(stripMarkup("[bold][red]error[/red][/bold]") == "error");
    CHECK(stripMarkup("[bright_green]ok[/]") == "ok");
    CHECK(stripMarkup("plain text") == "plain text");
    CHECK(stripMarkup("[unknown]kept[/unknown]") == "[unknown]kept[/unknown]");
}

TEST_CASE("stripMarkup: escaped brackets preserved", "[markup]") {
    CHECK(stripMarkup("\\[red]literal") == "[red]literal");
}

TEST_CASE("markupFormat: formats then applies markup", "[markup]") {
    auto result = markupFormat("[red]{} errors[/red]", 3);
    CHECK(result == "\033[31m3 errors\033[39m");
}

TEST_CASE("markupFormat: multiple arguments", "[markup]") {
    auto result = markupFormat("[bold]{}/{} [green]passed[/green][/bold]", 10, 12);
    CHECK(result == "\033[1m10/12 \033[32mpassed\033[39m\033[22m");
}

// --- Console tests ---

TEST_CASE("Console::println strips ANSI on non-TTY with Auto mode", "[markup]") {
    Console console;
    auto *tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    console.println(tmp, "[red]hello[/red] {}", "world");

    std::rewind(tmp);
    char buf[256] = {};
    auto *ret = std::fgets(buf, sizeof(buf), tmp);
    REQUIRE(ret != nullptr);
    std::fclose(tmp);

    CHECK(std::string(buf) == "hello world\n");
}

TEST_CASE("Console::println forces ANSI with Always mode", "[markup]") {
    Console console{ColorMode::Always};
    auto *tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    console.println(tmp, "[red]hi[/red]");

    std::rewind(tmp);
    char buf[256] = {};
    auto *ret = std::fgets(buf, sizeof(buf), tmp);
    REQUIRE(ret != nullptr);
    std::fclose(tmp);

    CHECK(std::string(buf) == "\033[31mhi\033[39m\n");
}

TEST_CASE("Console::println strips with Never mode", "[markup]") {
    Console console{ColorMode::Never};
    auto *tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    console.println(tmp, "[bold]text[/bold]");

    std::rewind(tmp);
    char buf[256] = {};
    auto *ret = std::fgets(buf, sizeof(buf), tmp);
    REQUIRE(ret != nullptr);
    std::fclose(tmp);

    CHECK(std::string(buf) == "text\n");
}

TEST_CASE("Console::setColorMode changes behavior", "[markup]") {
    Console console;
    console.setColorMode(ColorMode::Always);

    auto *tmp = std::tmpfile();
    REQUIRE(tmp != nullptr);

    console.println(tmp, "[green]ok[/green]");

    std::rewind(tmp);
    char buf[256] = {};
    auto *ret = std::fgets(buf, sizeof(buf), tmp);
    REQUIRE(ret != nullptr);
    std::fclose(tmp);

    CHECK(std::string(buf) == "\033[32mok\033[39m\n");
}

TEST_CASE("Console::format respects color mode", "[markup]") {
    Console always{ColorMode::Always};
    Console never{ColorMode::Never};

    CHECK(always.format("[red]x[/red]") == "\033[31mx\033[39m");
    CHECK(never.format("[red]x[/red]") == "x");
}
