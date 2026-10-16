#pragma once

// Native CLI policy only: third-party stdout chatter must not corrupt command
// output (in particular JSON). This changes process-wide descriptors, so callers
// must serialize imports. Do not use it in callable CLI or SDK import paths.
#include <cstdio>
#include <diagnostics.hpp>
#include <iostream>
#include <string_view>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace nodehammer::cli::detail {
class ImportLogGuard {
  public:
    ImportLogGuard(std::string_view errorCode, std::string_view errorMessage) {
        std::cout.flush();
        std::fflush(stdout);
#ifdef _WIN32
        saved_ = _dup(_fileno(stdout));
        const bool redirected = saved_ >= 0 && _dup2(_fileno(stderr), _fileno(stdout)) >= 0;
#else
        saved_ = dup(fileno(stdout));
        const bool redirected = saved_ >= 0 && dup2(fileno(stderr), fileno(stdout)) >= 0;
#endif
        if (!redirected) {
            closeSaved();
            throw Error{errorCode, errorMessage};
        }
    }
    ~ImportLogGuard() noexcept {
        // A stream configured to throw must not prevent descriptor restoration
        // while another exception is already unwinding the importer.
        try {
            std::cout.flush();
        } catch (...) {
        }
        std::fflush(stdout);
#ifdef _WIN32
        _dup2(saved_, _fileno(stdout));
#else
        dup2(saved_, fileno(stdout));
#endif
        closeSaved();
    }
    ImportLogGuard(const ImportLogGuard &) = delete;
    ImportLogGuard &operator=(const ImportLogGuard &) = delete;

  private:
    void closeSaved() noexcept {
        if (saved_ < 0)
            return;
#ifdef _WIN32
        _close(saved_);
#else
        close(saved_);
#endif
        saved_ = -1;
    }
    int saved_ = -1;
};
} // namespace nodehammer::cli::detail
