#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nodehammer::viewer {

/// Push-mode error/warning channel. Subsystems that produce diagnostics
/// (project filesystems, build session, importers) take an optional
/// `LogSink *` and emit at the moment a problem occurs, instead of
/// stashing a string for the consumer to poll.
///
/// The default no-op bodies let subsystems be wired without a sink and stay
/// silent. Sink lifetime must outlive the subsystem holding the pointer.
class LogSink {
  public:
    virtual ~LogSink() = default;

    virtual void warning(std::string_view) {}
    virtual void error(std::string_view) {}
};

/// In-memory `LogSink` that records every push for programmatic readback.
/// Useful when a caller needs to extract the diagnostic string after the
/// fact (CLI build paths funneling errors into a `Diagnostics`, tests
/// asserting that an error fired) rather than just printing or toasting.
class CapturingLogSink final : public LogSink {
  public:
    void warning(std::string_view msg) override { warnings_.emplace_back(msg); }
    void error(std::string_view msg) override { errors_.emplace_back(msg); }

    const std::vector<std::string> &errors() const noexcept { return errors_; }
    const std::vector<std::string> &warnings() const noexcept { return warnings_; }
    bool hasErrors() const noexcept { return !errors_.empty(); }

  private:
    std::vector<std::string> errors_;
    std::vector<std::string> warnings_;
};

/// Mixin base for subsystems that emit diagnostics through a `LogSink`.
/// Owns the sink pointer and a small buffer of pending diagnostics so that
/// constructors (or any push that runs before the App wires the sink) are
/// not silently dropped — they flush in order on the first non-null
/// `setLogSink`.
///
/// Subclasses call `pushError` / `pushWarning` and never touch the sink
/// pointer directly. Backends that must reach into the sink from a
/// non-method context (e.g. async fetch callbacks) override `setLogSink`
/// to plant a backpointer and route through `pushError` from the callback.
class LogSinkHolder {
  public:
    virtual ~LogSinkHolder() = default;

    /// Wire the diagnostic sink. Sink lifetime must outlive this holder.
    /// `nullptr` is fine — pushes go into the pending buffer until a real
    /// sink is wired. On a non-null sink, any buffered diagnostics flush
    /// in order before this call returns.
    virtual void setLogSink(LogSink *sink) noexcept {
        log_sink_ = sink;
        if (sink == nullptr) {
            return;
        }
        for (const auto &d : pending_diags_) {
            if (d.kind == PendingDiag::Kind::Error) {
                sink->error(d.msg);
            } else {
                sink->warning(d.msg);
            }
        }
        pending_diags_.clear();
        pending_diags_.shrink_to_fit();
    }

  protected:
    /// Push diagnostics from subclasses. Forwards to the wired `LogSink`
    /// when present; otherwise buffers up to `kPendingCap` entries until
    /// `setLogSink` lands. Subsystems emit at most a handful of diagnostics
    /// per failure path, so the cap never trips in practice.
    void pushError(std::string msg) {
        if (log_sink_ != nullptr) {
            log_sink_->error(msg);
            return;
        }
        if (pending_diags_.size() < kPendingCap) {
            pending_diags_.push_back({PendingDiag::Kind::Error, std::move(msg)});
        }
    }
    void pushWarning(std::string msg) {
        if (log_sink_ != nullptr) {
            log_sink_->warning(msg);
            return;
        }
        if (pending_diags_.size() < kPendingCap) {
            pending_diags_.push_back({PendingDiag::Kind::Warning, std::move(msg)});
        }
    }

  private:
    struct PendingDiag {
        enum class Kind { Warning, Error };
        Kind kind;
        std::string msg;
    };

    static constexpr std::size_t kPendingCap = 64;

    LogSink *log_sink_{nullptr};
    std::vector<PendingDiag> pending_diags_;
};

} // namespace nodehammer::viewer
