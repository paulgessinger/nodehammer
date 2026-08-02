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
// Deliberately C++20 and <iostream>: the installed headers must not require the
// C++23 the library itself is built with, and this file must not either — so no
// `std::print`, and no `std::format`.

#include <nodehammer/build.hpp>
#include <nodehammer/config.hpp>
#include <nodehammer/render_scene.hpp>
#include <nodehammer/semantic_scene.hpp>
#include <nodehammer/version.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int fail(const char *what, const nodehammer::DiagnosticList &diags) {
    std::cerr << what << '\n';
    for (const auto &d : diags) {
        std::cerr << "  [" << d.code << "] " << d.message << " (" << d.context << ")\n";
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
    std::cout << "consumer abi: sizeof(string)=" << sizeof(std::string)
              << " sizeof(vector<string>)=" << sizeof(std::vector<std::string>)
              << " sizeof(vector<byte>)=" << sizeof(std::vector<std::byte>);
#if defined(_MSC_VER)
    std::cout << " _MSC_VER=" << _MSC_VER << " _ITERATOR_DEBUG_LEVEL=" << _ITERATOR_DEBUG_LEVEL
              << " _MSVC_STL_VERSION=" << _MSVC_STL_VERSION;
#if defined(_DEBUG)
    std::cout << " _DEBUG";
#endif
#if defined(NDEBUG)
    std::cout << " NDEBUG";
#endif
#endif
    std::cout << " __cplusplus=" << static_cast<long>(__cplusplus) << std::endl;
}

int main() {
    reportAbi();

    // Header constant vs compiled-in value: a mismatch means headers from one
    // install and a library from another.
    const std::string_view linked = nodehammer::version();
    if (linked != nodehammer::VERSION) {
        std::cerr << "version mismatch: header says " << nodehammer::VERSION << ", library says "
                  << linked << '\n';
        return 1;
    }

    // The runtime capability query. These formats are unconditional, so a build
    // that cannot report them is broken rather than merely minimal.
    //
    // Dumped before it is judged: if an owning container does not survive the
    // boundary, what came back is the evidence, and reporting only "missing a
    // format" hides whether the vector was short, empty, or garbage.
    const auto semanticFormats = nodehammer::SemanticScene::formats();
    std::cout << "SemanticScene::formats() -> " << semanticFormats.size() << " entries\n";
    for (const auto &f : semanticFormats) {
        std::cout << "  '" << f << "' (len " << f.size() << ")\n";
    }
    std::cout << std::flush;

    if (!listed(semanticFormats, "synthetic") || !listed(semanticFormats, "flatbuffer")) {
        std::cerr << "SemanticScene::formats() is missing a built-in format\n";
        return 1;
    }
    if (!listed(nodehammer::Config::formats(), "toml") ||
        !listed(nodehammer::RenderScene::formats(), "gltf")) {
        std::cerr << "a built-in format is missing from formats()\n";
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
        std::cerr << "Config::checkString reported something about a sound document\n";
        return 1;
    }
    const auto report = nodehammer::Config::checkString("[[rules]]\nmatch = \"!!!\"\n");
    if (!report.hasErrors()) {
        std::cerr << "Config::checkString did not report a broken document\n";
        return 1;
    }
    std::cout << "Config::checkString reported " << report.size()
              << " problem(s) without throwing\n";

    // The file-taking face is a separate exported entry point from the string
    // one, so calling only `checkString` would leave it annotated on trust.
    const auto checkDir = std::filesystem::temp_directory_path() / "nh_shared_consumer_check";
    std::error_code checkEc;
    std::filesystem::create_directories(checkDir, checkEc);
    const auto checkPath = checkDir / "config.toml";
    {
        std::ofstream out{checkPath};
        out << "deduplicate_shapes = true\n";
    }
    if (!nodehammer::Config::check(checkPath).empty()) {
        std::cerr << "Config::check reported something about a sound document\n";
        return 1;
    }
    std::filesystem::remove_all(checkDir, checkEc);

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
        std::cerr << "build produced no triangles\n";
        return 1;
    }

    // Round-trip through the wire form — the path that reaches flatbuffers, one
    // of the private static dependencies the shared library must have absorbed.
    const std::vector<std::byte> bytes = rendered.scene.toNhr();
    const auto reread = nodehammer::RenderScene::read(bytes);
    if (reread.triangleCount() != rendered.scene.triangleCount()) {
        std::cerr << "RenderScene byte round-trip failed\n";
        return 1;
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
        std::cerr << "the decomposed pipeline disagreed with build()\n";
        return 1;
    }

    const std::vector<std::byte> nhb = imported.scene.toNhb();
    if (nhb.empty()) {
        std::cerr << "toNhb() produced nothing\n";
        return 1;
    }

    // Counts: reached rather than checked, beyond the one invariant that a
    // scene with triangles has meshes and materials to bind them.
    std::cout << "semantic: " << imported.scene.nodeCount() << " nodes, "
              << imported.scene.logVolCount() << " logVols, " << imported.scene.shapeCount()
              << " shapes, " << imported.scene.materialCount() << " materials\n";
    std::cout << "render:   " << rendered.scene.nodeCount() << " nodes, "
              << rendered.scene.meshCount() << " meshes, " << rendered.scene.materialCount()
              << " materials, " << rendered.scene.triangleCount() << " triangles\n";
    if (rendered.scene.meshCount() == 0 || rendered.scene.materialCount() == 0) {
        std::cerr << "a scene with triangles reported no meshes or materials\n";
        return 1;
    }

    // Both write paths, through the output slice, into a directory the consumer
    // is allowed to create. `Config::output()` is reached nowhere else.
    const auto outDir = std::filesystem::temp_directory_path() / "nh_shared_consumer";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    const auto semanticOut = outDir / "scene.nhb";
    const auto renderOut = outDir / "scene.glb";
    // Neither returns anything: they wrote the file or they threw, so the file
    // existing afterwards is the whole check.
    imported.scene.write(semanticOut);
    rendered.scene.write(renderOut, config.config.output());
    if (!std::filesystem::exists(semanticOut) || !std::filesystem::exists(renderOut)) {
        std::cerr << "a write returned without producing a file\n";
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
        // Every accessor, not just the two that make a nice message: on Windows
        // there is no export table to inspect, so calling one is the only thing
        // that proves it carries its NH_API.
        const nodehammer::Diagnostic asDiagnostic = e.diagnostic();
        std::cout << "caught Error across the boundary: [" << e.code() << "] " << e.what()
                  << " (context '" << e.context() << "', severity "
                  << static_cast<int>(asDiagnostic.severity) << ", " << e.observed().size()
                  << " observed)\n";
    } catch (const std::exception &e) {
        std::cerr << "caught the wrong type across the boundary: " << e.what() << '\n';
        return 1;
    }
    if (!threw) {
        std::cerr << "a failed read did not throw\n";
        return 1;
    }

    // Both constructors: a consumer that wraps this library may want to raise
    // the same type, and the four-argument one carries a list along.
    nodehammer::DiagnosticList carried;
    carried.add({nodehammer::Diagnostic::Severity::Warning, "NH0000", "constructed by a consumer"});
    const nodehammer::Error plain{"NH0000", "constructed by a consumer"};
    const nodehammer::Error withList{"NH0000", "constructed by a consumer", "context", carried};
    if (plain.observed().size() != 0 || withList.observed().size() != carried.size()) {
        std::cerr << "Error did not carry its observations across the boundary\n";
        return 1;
    }

    std::cout << "nodehammer " << linked << ": " << rendered.scene.nodeCount() << " nodes, "
              << rendered.scene.triangleCount() << " triangles" << std::endl;
    return 0;
}
