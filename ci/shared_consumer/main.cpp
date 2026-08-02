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
#include <exception>
#include <filesystem>
#include <span>
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

bool listed(std::span<const std::string_view> names, std::string_view needle) {
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
        std::printf("  '%.*s' (len %zu)\n", static_cast<int>(f.size()), f.data(), f.size());
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

    // The other promise over the same collector: a document that `parse` would
    // have thrown about is `checkString`'s ordinary answer. Both are exported,
    // so both are called here — that is what this consumer is for.
    if (!nodehammer::Config::checkString("deduplicate_shapes = true\n").empty()) {
        std::puts("Config::checkString reported something about a sound document");
        return 1;
    }
    const auto report = nodehammer::Config::checkString("[[rules]]\nmatch = \"!!!\"\n");
    if (!report.hasErrors()) {
        std::puts("Config::checkString did not report a broken document");
        return 1;
    }
    std::printf("Config::checkString reported %zu problem(s) without throwing\n", report.size());

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

    // ── Every remaining exported entry point ─────────────────────────────────
    // Not for the behaviour — the unit tests cover that far better — but because
    // calling one is the only way to prove it is *reachable*. A member that is
    // missing its NH_API links fine in-tree against the static archive and fails
    // only here, for an external consumer of the shared library. Anything the
    // consumer never calls is annotated on trust rather than checked, so it
    // calls all of them.
    const auto selected = nodehammer::applySelection(imported.scene, config.config.scene());
    const auto deduped = nodehammer::deduplicate(selected.scene, config.config.scene());
    const auto lowered = nodehammer::tessellate(deduped.scene, config.config.scene());
    if (selected.diags.hasErrors() || deduped.diags.hasErrors() || lowered.diags.hasErrors()) {
        return fail("a pipeline verb failed", lowered.diags);
    }
    if (lowered.scene.triangleCount() != rendered.scene.triangleCount()) {
        std::fprintf(stderr, "the decomposed pipeline disagreed with build()\n");
        return 1;
    }

    const std::vector<std::byte> nhb = imported.scene.toNhb();
    if (nhb.empty()) {
        std::fprintf(stderr, "toNhb() produced nothing\n");
        return 1;
    }

    // Counts: reached rather than checked, beyond the one invariant that a
    // scene with triangles has meshes and materials to bind them.
    std::printf("semantic: %zu nodes, %zu logVols, %zu shapes, %zu materials\n",
                imported.scene.nodeCount(), imported.scene.logVolCount(),
                imported.scene.shapeCount(), imported.scene.materialCount());
    std::printf("render:   %zu nodes, %zu meshes, %zu materials, %zu triangles\n",
                rendered.scene.nodeCount(), rendered.scene.meshCount(),
                rendered.scene.materialCount(), rendered.scene.triangleCount());
    if (rendered.scene.meshCount() == 0 || rendered.scene.materialCount() == 0) {
        std::fprintf(stderr, "a scene with triangles reported no meshes or materials\n");
        return 1;
    }

    // Both write paths, through the output slice, into a directory the consumer
    // is allowed to create. `Config::output()` is reached nowhere else.
    const auto outDir = std::filesystem::temp_directory_path() / "nh_shared_consumer";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const auto semanticOut = outDir / "scene.nhb";
    const auto renderOut = outDir / "scene.glb";
    const auto semanticDiags = imported.scene.write(semanticOut);
    if (semanticDiags.hasErrors()) {
        return fail("SemanticScene::write failed", semanticDiags);
    }
    const auto renderDiags = rendered.scene.write(renderOut, config.config.output());
    if (renderDiags.hasErrors()) {
        return fail("RenderScene::write failed", renderDiags);
    }
    if (!std::filesystem::exists(semanticOut) || !std::filesystem::exists(renderOut)) {
        std::fprintf(stderr, "a write reported success but produced no file\n");
        return 1;
    }
    std::filesystem::remove_all(outDir, ec);

    // An unreadable input has to arrive as a thrown `Error` — which is the only
    // check here that exercises an exception crossing the library boundary, the
    // thing that needs the type's identity to be exported (NH_API_TYPE) and the
    // two sides' runtimes to agree.
    bool threw = false;
    try {
        (void)nodehammer::SemanticScene::read("/nodehammer/definitely/not/here.nhb");
    } catch (const nodehammer::Error &e) {
        threw = true;
        std::printf("caught Error across the boundary: [%s] %s\n", e.code().c_str(), e.what());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "caught the wrong type across the boundary: %s\n", e.what());
        return 1;
    }
    if (!threw) {
        std::fprintf(stderr, "a failed read did not throw\n");
        return 1;
    }

    std::printf("nodehammer %.*s: %zu nodes, %zu triangles\n", static_cast<int>(linked.size()),
                linked.data(), rendered.scene.nodeCount(), rendered.scene.triangleCount());
    return 0;
}
