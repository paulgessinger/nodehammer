#include <nodehammer/nhb.hpp>
#include <nodehammer/tgeo.hpp>
#pragma once

// Both programs compile this body against the public API, linking either the
// minimal ingestion shared library or the full library. Compare their .nhb output;
// computed world transforms are checked separately in core.cpp.

#include "geometry.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

inline int goldenMain(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <out.nhb>\n", argv[0]);
        return EXIT_FAILURE;
    }

    TGeoManager *mgr = makeGoldenGeometry();
    const auto result = nodehammer::fromTGeo(*mgr);

    if (!result.scene.valid()) {
        std::fprintf(stderr, "import produced no scene\n");
        return EXIT_FAILURE;
    }
    if (result.diags.hasErrors()) {
        std::fprintf(stderr, "import reported errors\n");
        return EXIT_FAILURE;
    }

    const std::vector<std::byte> nhb = nodehammer::toNhb(result.scene);
    if (nhb.empty()) {
        std::fprintf(stderr, "toNhb produced no bytes\n");
        return EXIT_FAILURE;
    }

    // The minimal library produces bytes; file ownership stays with the caller.
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
