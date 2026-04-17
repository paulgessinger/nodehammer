#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <nodehammer/detail/timing.hpp>

using namespace std::chrono_literals;
using nodehammer::detail::formatDuration;
using nodehammer::detail::Timer;
using nodehammer::detail::TimingReport;

TEST_CASE("formatDuration: picks the right unit", "[timing]") {
    CHECK(formatDuration(std::chrono::nanoseconds{0}) == "0 ns");
    CHECK(formatDuration(std::chrono::nanoseconds{999}) == "999 ns");
    CHECK(formatDuration(std::chrono::nanoseconds{1'000}) == "1.00 µs");
    CHECK(formatDuration(std::chrono::nanoseconds{1'500}) == "1.50 µs");
    CHECK(formatDuration(std::chrono::nanoseconds{1'000'000}) == "1.00 ms");
    CHECK(formatDuration(std::chrono::milliseconds{250}) == "250.00 ms");
    CHECK(formatDuration(std::chrono::seconds{2}) == "2.00 s");
    CHECK(formatDuration(std::chrono::seconds{59}) == "59.00 s");
    CHECK(formatDuration(std::chrono::seconds{61}).starts_with("1m"));
}

TEST_CASE("Timer: elapsed advances monotonically", "[timing]") {
    Timer t;
    const auto a = t.elapsed();
    std::this_thread::sleep_for(1ms);
    const auto b = t.elapsed();
    CHECK(b >= a);
    CHECK(b >= 500'000ns); // slept ≥1 ms, allow slop for CI timers

    t.reset();
    const auto c = t.elapsed();
    CHECK(c < b);
}

TEST_CASE("TimingReport::record preserves insertion order and sums total", "[timing]") {
    TimingReport r;
    CHECK(r.empty());
    r.record("a", 100ns);
    r.record("b", 250ns);
    r.record("a", 50ns); // duplicate labels are kept as-is

    REQUIRE(r.entries().size() == 3);
    CHECK(r.entries()[0].label == "a");
    CHECK(r.entries()[1].label == "b");
    CHECK(r.entries()[2].label == "a");
    CHECK(r.total() == 400ns);
    CHECK_FALSE(r.empty());
}

TEST_CASE("TimingReport::Scope records on destruction", "[timing]") {
    TimingReport r;
    {
        auto s = r.scope("work");
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(r.entries().size() == 1);
    CHECK(r.entries()[0].label == "work");
    CHECK(r.entries()[0].duration >= 500'000ns);
}

TEST_CASE("TimingReport::Scope::dismiss suppresses recording", "[timing]") {
    TimingReport r;
    {
        auto s = r.scope("skipped");
        s.dismiss();
    }
    CHECK(r.entries().empty());
}

TEST_CASE("TimingReport::Scope move transfers ownership", "[timing]") {
    TimingReport r;
    {
        auto s1 = r.scope("moved");
        auto s2 = std::move(s1);
    }
    REQUIRE(r.entries().size() == 1);
    CHECK(r.entries()[0].label == "moved");
}
