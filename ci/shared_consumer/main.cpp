// Stand-in for an external consumer: the only place the API is exercised with
// no access to the source tree, no dependency hints and no include paths beyond
// nodehammer's own install prefix.
//
// It links *and runs*, which is the point — compiling alone proves little, since
// headers install whether or not the library does. Reaching an out-of-line
// symbol means the export table, the SONAME and the Windows import library all
// resolved; running the pipeline means the private static dependencies (zstd,
// flatbuffers, manifold, toml++) really are inside the shared object rather than
// left for someone else to link.
//
// Deliberately C++20 and <cstdio>: the installed headers must not require the
// C++23 the library itself is built with.

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/version.hpp>

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(const char *what, const nodehammer::DiagnosticList &diags) {
    std::fprintf(stderr, "%s\n", what);
    for (const auto &d : diags) {
        std::fprintf(stderr, "  [%s] %s (%s)\n", d.code.c_str(), d.message.c_str(),
                     d.context.c_str());
    }
    return 1;
}

bool listed(const std::vector<std::string> &names, std::string_view needle) {
    for (const auto &n : names) {
        if (n == needle) {
            return true;
        }
    }
    return false;
}

} // namespace

// What the consumer's own standard library looks like. Printed unconditionally
// because an owning container crossing a shared-library boundary is only safe
// while both sides agree on its layout, and on MSVC that agreement is not
// implied by anything a consumer writes: `std::string` carries an extra proxy
// pointer when `_ITERATOR_DEBUG_LEVEL != 0`, so the two sides can differ in
// `sizeof` while compiling and linking cleanly.
void reportAbi() {
    std::printf("consumer abi: sizeof(string)=%zu sizeof(vector<string>)=%zu"
                " sizeof(vector<byte>)=%zu",
                sizeof(std::string), sizeof(std::vector<std::string>),
                sizeof(std::vector<std::byte>));
#if defined(_MSC_VER)
    std::printf(" _MSC_VER=%d _ITERATOR_DEBUG_LEVEL=%d _MSVC_STL_VERSION=%d", _MSC_VER,
                _ITERATOR_DEBUG_LEVEL, _MSVC_STL_VERSION);
#if defined(_DEBUG)
    std::printf(" _DEBUG");
#endif
#if defined(NDEBUG)
    std::printf(" NDEBUG");
#endif
#endif
    std::printf(" __cplusplus=%ld\n", static_cast<long>(__cplusplus));
    std::fflush(stdout);
}

int main() {
    reportAbi();

    // Header constant vs compiled-in value: a mismatch means headers from one
    // install and a library from another.
    const std::string_view linked = nodehammer::version();
    if (linked != nodehammer::VERSION) {
        std::fprintf(stderr, "version mismatch: header says %.*s, library says %.*s\n",
                     static_cast<int>(nodehammer::VERSION.size()), nodehammer::VERSION.data(),
                     static_cast<int>(linked.size()), linked.data());
        return 1;
    }

    // The runtime capability query. These formats are unconditional, so a build
    // that cannot report them is broken rather than merely minimal.
    //
    // Dumped before it is judged: if an owning container does not survive the
    // boundary, what came back is the evidence, and reporting only "missing a
    // format" hides whether the vector was short, empty, or garbage.
    const auto semanticFormats = nodehammer::SemanticScene::formats();
    std::printf("SemanticScene::formats() -> %zu entries\n", semanticFormats.size());
    for (const auto &f : semanticFormats) {
        std::printf("  '%s' (len %zu)\n", f.c_str(), f.size());
    }
    std::fflush(stdout);

    if (!listed(semanticFormats, "synthetic") || !listed(semanticFormats, "flatbuffer")) {
        std::fprintf(stderr, "SemanticScene::formats() is missing a built-in format\n");
        return 1;
    }
    if (!listed(nodehammer::Config::formats(), "toml") ||
        !listed(nodehammer::RenderScene::formats(), "gltf")) {
        std::fprintf(stderr, "a built-in format is missing from formats()\n");
        return 1;
    }

    // A config parsed from a string with no location, so its (absent) includes
    // would resolve against nothing rather than against this process's working
    // directory.
    const auto config = nodehammer::Config::parse("deduplicate_shapes = true\n"
                                                  "[export.glb]\n"
                                                  "unit_scale = 1.0\n");
    if (config.diags.hasErrors() || !config.config.valid()) {
        return fail("Config::parse failed", config.diags);
    }

    // The synthetic importer ignores its path, so the whole pipeline runs
    // without the consumer needing a geometry file to point at.
    const auto imported =
        nodehammer::SemanticScene::read("", nodehammer::SemanticScene::ReadOptions{"synthetic"});
    if (imported.diags.hasErrors() || !imported.scene.valid()) {
        return fail("SemanticScene::read failed", imported.diags);
    }

    const auto rendered = nodehammer::build(imported.scene, config.config.scene());
    if (rendered.diags.hasErrors() || !rendered.scene.valid()) {
        return fail("build failed", rendered.diags);
    }
    if (rendered.scene.triangleCount() == 0) {
        std::fprintf(stderr, "build produced no triangles\n");
        return 1;
    }

    // Round-trip through the wire form — the path that reaches flatbuffers, one
    // of the private static dependencies the shared library must have absorbed.
    const std::vector<std::byte> bytes = rendered.scene.toNhr();
    const auto reread = nodehammer::RenderScene::read(bytes);
    if (reread.diags.hasErrors() ||
        reread.scene.triangleCount() != rendered.scene.triangleCount()) {
        return fail("RenderScene byte round-trip failed", reread.diags);
    }

    // And a failure that must arrive as a diagnostic rather than as an exception
    // crossing the ABI. The handle still refers to a scene — an empty one —
    // because the diagnostics, not `valid()`, are what report the failure.
    const auto missing = nodehammer::SemanticScene::read("/nodehammer/definitely/not/here.nhb");
    if (!missing.diags.hasErrors() || missing.scene.nodeCount() != 0) {
        std::fprintf(stderr, "a failed read did not report an error\n");
        return 1;
    }

    std::printf("nodehammer %.*s: %zu nodes, %zu triangles\n", static_cast<int>(linked.size()),
                linked.data(), rendered.scene.nodeCount(), rendered.scene.triangleCount());
    return 0;
}
