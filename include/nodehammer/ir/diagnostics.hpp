#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace nodehammer {

enum class DiagnosticSeverity {
    Info,
    Warning,
    Error,
    Fatal,
};

[[nodiscard]] constexpr std::string_view severityName(DiagnosticSeverity s) noexcept {
    switch (s) {
    case DiagnosticSeverity::Info:
        return "info";
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        return "error";
    case DiagnosticSeverity::Fatal:
        return "fatal";
    }
    return "unknown";
}

struct Diagnostic {
    DiagnosticSeverity severity{DiagnosticSeverity::Info};
    std::string code; ///< NH-series code, e.g. "NH0301"
    std::string message;
    std::string context; ///< Optional: node name, file path, etc.

    [[nodiscard]] bool isFatal() const noexcept { return severity == DiagnosticSeverity::Fatal; }
};

inline void to_json(nlohmann::json &j, const Diagnostic &d) {
    j = nlohmann::json{
        {"severity", severityName(d.severity)},
        {"code", d.code},
        {"message", d.message},
    };
    if (!d.context.empty()) {
        j["context"] = d.context;
    }
}

class DiagnosticList {
  public:
    void add(Diagnostic d) { items_.push_back(std::move(d)); }

    void info(std::string code, std::string message, std::string context = {}) {
        add({DiagnosticSeverity::Info, std::move(code), std::move(message), std::move(context)});
    }
    void warn(std::string code, std::string message, std::string context = {}) {
        add({DiagnosticSeverity::Warning, std::move(code), std::move(message), std::move(context)});
    }
    void error(std::string code, std::string message, std::string context = {}) {
        add({DiagnosticSeverity::Error, std::move(code), std::move(message), std::move(context)});
    }
    void fatal(std::string code, std::string message, std::string context = {}) {
        add({DiagnosticSeverity::Fatal, std::move(code), std::move(message), std::move(context)});
    }

    void append(const DiagnosticList &other) {
        for (const auto &d : other.items_) {
            items_.push_back(d);
        }
    }

    [[nodiscard]] bool hasFatal() const noexcept {
        for (const auto &d : items_) {
            if (d.isFatal())
                return true;
        }
        return false;
    }

    [[nodiscard]] bool hasErrors() const noexcept {
        for (const auto &d : items_) {
            if (d.severity >= DiagnosticSeverity::Error)
                return true;
        }
        return false;
    }

    [[nodiscard]] const std::vector<Diagnostic> &items() const noexcept { return items_; }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }

    [[nodiscard]] nlohmann::json toJson() const {
        auto arr = nlohmann::json::array();
        for (const auto &d : items_) {
            arr.push_back(d);
        }
        return arr;
    }

  private:
    std::vector<Diagnostic> items_;
};

inline void to_json(nlohmann::json &j, const DiagnosticList &dl) { j = dl.toJson(); }

} // namespace nodehammer
