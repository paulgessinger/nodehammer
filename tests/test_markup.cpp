#include <catch2/catch_test_macros.hpp>
#include <nodehammer/markup.hpp>

using nodehammer::markup;
using nodehammer::markup_format;
using nodehammer::strip_markup;

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

TEST_CASE("strip_markup: removes tags", "[markup]") {
    CHECK(strip_markup("[bold][red]error[/red][/bold]") == "error");
    CHECK(strip_markup("[bright_green]ok[/]") == "ok");
    CHECK(strip_markup("plain text") == "plain text");
    CHECK(strip_markup("[unknown]kept[/unknown]") == "[unknown]kept[/unknown]");
}

TEST_CASE("strip_markup: escaped brackets preserved", "[markup]") {
    CHECK(strip_markup("\\[red]literal") == "[red]literal");
}

TEST_CASE("markup_format: formats then applies markup", "[markup]") {
    auto result = markup_format("[red]{} errors[/red]", 3);
    CHECK(result == "\033[31m3 errors\033[39m");
}

TEST_CASE("markup_format: multiple arguments", "[markup]") {
    auto result = markup_format("[bold]{}/{} [green]passed[/green][/bold]", 10, 12);
    CHECK(result == "\033[1m10/12 \033[32mpassed\033[39m\033[22m");
}
