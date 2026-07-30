#pragma once

// Minimal OBJ writer for visual inspection of tessellation output.
// Not intended for production use — only included in test targets.
//
// writeObjToDir(out, name) writes <name>.obj to NODEHAMMER_TESS_OBJ_DIR when
// that macro is defined at compile time. When the macro is absent the call
// compiles to a no-op, so tests can call it unconditionally.

#include <tessellation/tessellator.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace nodehammer::test {

/// Write a TessellationOutput to an OBJ file at the given path.
/// Exports positions (v), normals (vn), and indexed triangles (f).
inline void writeObj(const tessellation::TessellationOutput &out,
                     const std::filesystem::path &path) {
    std::ofstream f{path};

    for (const auto &v : out.vertices) {
        f << "v " << v.position.x << " " << v.position.y << " " << v.position.z << "\n";
    }

    for (const auto &v : out.vertices) {
        f << "vn " << v.normal.x << " " << v.normal.y << " " << v.normal.z << "\n";
    }

    // OBJ indices are 1-based; faces reference v//vn (no UVs)
    for (std::size_t i = 0; i + 2 < out.indices.size(); i += 3) {
        const auto i0 = out.indices[i + 0] + 1;
        const auto i1 = out.indices[i + 1] + 1;
        const auto i2 = out.indices[i + 2] + 1;
        f << "f " << i0 << "//" << i0 << " " << i1 << "//" << i1 << " " << i2 << "//" << i2 << "\n";
    }
}

/// Write out to NODEHAMMER_TESS_OBJ_DIR/<name>.obj when that macro is defined.
/// Compiles to a no-op otherwise.
inline void writeObjToDir(const tessellation::TessellationOutput &out, const std::string &name) {
#ifdef NODEHAMMER_TESS_OBJ_DIR
    const std::filesystem::path dir{NODEHAMMER_TESS_OBJ_DIR};
    std::filesystem::create_directories(dir);
    writeObj(out, dir / (name + ".obj"));
#else
    (void)out;
    (void)name;
#endif
}

} // namespace nodehammer::test
