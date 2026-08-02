#pragma once

// Shared helpers for the public-API suite.
//
// Every header this suite includes is one an installed consumer has. Nothing
// from src/ appears anywhere in tests/public/, and that is the point of the
// target rather than a style rule: `nodehammer_public_tests` links
// `nodehammer_shared`, so it sees neither NH_STATIC nor NH_EXPORTS and reaches
// the library by exactly the path an external consumer does — dllimport on
// Windows, the dynamic symbol table elsewhere. A member missing its NH_API
// links fine against the archive every other test uses and fails only here.
//
// The discipline is enforced rather than trusted: an internal header would
// still *compile* (the shared target propagates src/ on the include path
// in-tree), but its symbols are hidden in the shared object, so the link fails.
//
// What this suite is not: a second copy of tests/api/. Those cover behaviour
// against the archive, with internal headers to check the results against. This
// one covers the surface — that every exported entry point is reachable, and
// that what it hands back survives the boundary intact.

#include <nodehammer/diagnostics.hpp>
#include <nodehammer/semantic_scene.hpp>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace nhtest {

/// A directory that removes itself. This suite writes real files — a config to
/// read back, a scene to round-trip — and none of it may outlive the case.
class TempDir {
  public:
    explicit TempDir(std::string_view name)
        : path_(std::filesystem::temp_directory_path() /
                ("nh_public_" + std::string{name} + "_" + std::to_string(next()))) {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] std::filesystem::path operator/(std::string_view leaf) const {
        return path_ / leaf;
    }

    /// Write `content` to `leaf` and hand back the path.
    std::filesystem::path put(std::string_view leaf, std::string_view content) const {
        const auto p = path_ / leaf;
        std::ofstream out{p, std::ios::binary};
        out << content;
        return p;
    }

  private:
    // Distinct names per instance: cases may run in the same process and two
    // TempDirs with one name would delete each other's contents.
    static unsigned next() {
        static unsigned n = 0;
        return n++;
    }

    std::filesystem::path path_;
};

/// The scene nearly every case starts from.
///
/// The synthetic importer ignores its path, so the suite needs no geometry
/// fixture on disk — it exercises the same entry points an installed consumer
/// would with none of the fixture wiring. It builds a single box: one node, one
/// logical volume, one shape, one material, which is what lets the counts below
/// be exact rather than merely non-zero.
inline nodehammer::SemanticScene syntheticScene() {
    return nodehammer::SemanticScene::read("", nodehammer::SemanticScene::ReadOptions{"synthetic"})
        .scene;
}

inline bool listed(std::span<const std::string_view> names, std::string_view needle) {
    for (const auto &n : names) {
        if (n == needle) {
            return true;
        }
    }
    return false;
}

/// True when any entry carries `Fatal`. Not `DiagnosticList::hasErrors`, which
/// answers a different question — this one guards the invariant that a returned
/// list never carries the severity reserved for `Error::diagnostic()`.
inline bool anyFatal(const nodehammer::DiagnosticList &diags) {
    for (const auto &d : diags) {
        if (d.severity == nodehammer::Diagnostic::Severity::Fatal) {
            return true;
        }
    }
    return false;
}

} // namespace nhtest
