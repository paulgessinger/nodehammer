#include <ir/fb/render/exporter.hpp>

#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/fb/render/flatbuffer.hpp>

#include <format>

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
    //
    // Serialisation and the write itself throw plain std::exception; every
    // caller that reaches an exporter directly — `convert` among them — reports
    // `Error` and nothing else, so the translation happens here rather than at
    // each of them, as the other FlatBuffer exporter does.
    try {
        detail::zstd_io::writeBytesToFile(path, renderSceneToBytes(scene));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &ex) {
        throw Error{
            codes::kFatalExportWriteFailed,
            std::format("failed to write render FlatBuffer '{}': {}", path.string(), ex.what()),
            path.string()};
    }
}

} // namespace nodehammer::ir
