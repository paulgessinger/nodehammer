#include <ir/fb/semantic/importer.hpp>

#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>

#include <format>
#include <span>

namespace nodehammer::ir {

std::string_view FlatBufferImporter::formatName() const noexcept { return "flatbuffer"; }

std::vector<std::string> FlatBufferImporter::supportedExtensions() const {
    return {"nhb", "nhb.zst"};
}

ImportResult FlatBufferImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    try {
        auto raw = detail::zstd_io::readBytesFromFile(path);
        auto scene = semanticSceneFromBytes(raw);
        scene.computeWorldTransforms();
        scene.computeOriginalPaths();
        result.scene = std::move(scene);
    } catch (const std::exception &ex) {
        result.diags.error(
            codes::kErrImportFileNotFound,
            std::format("failed to load FlatBuffer '{}': {}", path.string(), ex.what()));
    }

    return result;
}

ImportResult FlatBufferImporter::importFromBytes(std::string_view filename,
                                                 std::span<const std::byte> bytes) {
    ImportResult result;

    try {
        // `.zst` suffix on the filename signals zstd-compressed input —
        // same convention as the path-based reader, just decided from
        // the filename rather than the path.
        const bool zst = detail::zstd_io::hasZstdExtension(std::filesystem::path{filename});
        std::vector<std::byte> decompressed;
        std::span<const std::byte> raw = bytes;
        if (zst) {
            decompressed = detail::zstd_io::decompress(bytes);
            raw = std::span<const std::byte>{decompressed};
        }
        auto scene = semanticSceneFromBytes(raw);
        scene.computeWorldTransforms();
        scene.computeOriginalPaths();
        result.scene = std::move(scene);
    } catch (const std::exception &ex) {
        result.diags.error(codes::kErrImportFileNotFound,
                           std::format("failed to load FlatBuffer '{}': {}", filename, ex.what()));
    }

    return result;
}

} // namespace nodehammer::ir
