#pragma once

// The body of both golden-comparison programs.
//
// Included *after* whichever declaration of the API the including translation
// unit uses — the amalgamated header on one side, `<nodehammer/semantic_scene.hpp>`
// on the other. Sharing the body is the point: if the two programs differed in
// any way beyond how they reach `nodehammer::`, a byte difference would no
// longer mean what the test claims it means.
//
// Writes `.nhb` bytes to argv[1] and nothing else. No formatting, no summary,
// no path recorded inside the payload — the comparison is on the file.

#include "golden_geometry.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

inline int goldenMain(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <out.nhb>\n", argv[0]);
        return EXIT_FAILURE;
    }

    TGeoManager *mgr = makeGoldenGeometry();
    const auto result = nodehammer::SemanticScene::read(*mgr);

    if (!result.scene.valid()) {
        std::fprintf(stderr, "import produced no scene\n");
        return EXIT_FAILURE;
    }
    if (result.diags.hasErrors()) {
        std::fprintf(stderr, "import reported errors\n");
        return EXIT_FAILURE;
    }

    const std::vector<std::byte> nhb = result.scene.toNhb();
    if (nhb.empty()) {
        std::fprintf(stderr, "toNhb produced no bytes\n");
        return EXIT_FAILURE;
    }

    // Written through stdio rather than the library's own file helpers: those
    // are not in the connector's slice, and the amalgamated side must not need
    // anything the shipped header does not carry.
    std::FILE *out = std::fopen(argv[1], "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    const std::size_t written = std::fwrite(nhb.data(), 1, nhb.size(), out);
    const int closed = std::fclose(out);
    if (written != nhb.size() || closed != 0) {
        std::fprintf(stderr, "short write to %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    std::fprintf(stderr, "wrote %zu bytes to %s\n", nhb.size(), argv[1]);
    return EXIT_SUCCESS;
}
