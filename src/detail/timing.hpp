#pragma once

#include <chrono>
#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodehammer::detail {

/// Format a duration as human-readable text (ns / μs / ms / s).
inline std::string formatDuration(std::chrono::nanoseconds d) {
    using namespace std::chrono;
    const auto ns = d.count();
    if (ns < 1'000) {
        return std::format("{} ns", ns);
    }
    if (ns < 1'000'000) {
        return std::format("{:.2f} µs", static_cast<double>(ns) / 1'000.0);
    }
    if (ns < 1'000'000'000) {
        return std::format("{:.2f} ms", static_cast<double>(ns) / 1'000'000.0);
    }
    const double s = static_cast<double>(ns) / 1'000'000'000.0;
    if (s < 60.0) {
        return std::format("{:.2f} s", s);
    }
    const auto total_s = static_cast<long long>(s);
    const auto mins = total_s / 60;
    const auto secs = s - static_cast<double>(mins * 60);
    return std::format("{}m {:.2f}s", mins, secs);
}

/// Monotonic timer backed by `std::chrono::steady_clock`.
class Timer {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Timer() noexcept : m_start(Clock::now()) {}

    void reset() noexcept { m_start = Clock::now(); }

    std::chrono::nanoseconds elapsed() const noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_start);
    }

  private:
    TimePoint m_start;
};

/// Collects named timing samples in insertion order and prints a summary.
///
/// Use `scope(label)` to time a block via RAII, or `record(label, d)` to
/// append an already-measured duration.
class TimingReport {
  public:
    struct Entry {
        std::string label;
        std::chrono::nanoseconds duration;
    };

    void record(std::string label, std::chrono::nanoseconds duration) {
        m_entries.push_back({std::move(label), duration});
    }

    const std::vector<Entry> &entries() const noexcept { return m_entries; }

    std::chrono::nanoseconds total() const noexcept {
        std::chrono::nanoseconds sum{0};
        for (const auto &e : m_entries) {
            sum += e.duration;
        }
        return sum;
    }

    bool empty() const noexcept { return m_entries.empty(); }

    /// RAII helper that records `label` with the elapsed lifetime on destruction.
    /// The report must outlive the scope.
    class Scope {
      public:
        Scope(TimingReport &report, std::string label)
            : m_report(&report), m_label(std::move(label)) {}

        ~Scope() {
            if (m_report != nullptr) {
                m_report->record(std::move(m_label), m_timer.elapsed());
            }
        }

        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

        Scope(Scope &&other) noexcept
            : m_report(other.m_report), m_label(std::move(other.m_label)), m_timer(other.m_timer) {
            other.m_report = nullptr;
        }
        Scope &operator=(Scope &&) = delete;

        /// Abandon without recording (useful for conditional timing).
        void dismiss() noexcept { m_report = nullptr; }

      private:
        TimingReport *m_report;
        std::string m_label;
        Timer m_timer;
    };

    [[nodiscard]] Scope scope(std::string label) { return Scope{*this, std::move(label)}; }

    /// Print one line per entry plus a total. Widths align to the longest label.
    void print(FILE *f = stderr, std::string_view title = "Timings") const {
        if (m_entries.empty()) {
            return;
        }
        std::size_t width = 0;
        for (const auto &e : m_entries) {
            width = std::max(width, e.label.size());
        }
        std::println(f, "{}:", title);
        for (const auto &e : m_entries) {
            std::println(f, "  {:<{}}  {}", e.label, width, formatDuration(e.duration));
        }
        std::println(f, "  {:<{}}  {}", "total", width, formatDuration(total()));
    }

  private:
    std::vector<Entry> m_entries;
};

} // namespace nodehammer::detail
