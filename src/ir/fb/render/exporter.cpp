#include <detail/zstd_io.hpp>
#include <ir/fb/render/exporter.hpp>
#include <ir/fb/render/flatbuffer.hpp>

namespace nodehammer::ir {

std::string_view RenderFlatbufferExporter::formatName() const noexcept { return "nhr"; }

std::vector<std::string> RenderFlatbufferExporter::supportedExtensions() const {
    return {".nhr", ".nhr.zst"};
}

void RenderFlatbufferExporter::write(const render::Scene &scene, const std::filesystem::path &path,
                                     const ExportConfig &config) const {
    // `config` carries the `[export.*]` tuning the mesh exporters read — units,
    // axis conventions, material overrides. None of it applies to a format whose
    // whole point is to be the render IR unchanged: anything it altered would
    // make the round trip lossy, which is the one property this format has.
    (void)config;
    // Compression is decided by the path, inside writeBytesToFile, so `.nhr` and
    // `.nhr.zst` take the same path here and differ only where they are named.
    detail::zstd_io::writeBytesToFile(path, renderSceneToBytes(scene));
}

} // namespace nodehammer::ir
