#include <ir/fb/semantic/importer.hpp>

#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>

#include <cstring>
#include <format>
#include <span>

namespace nodehammer::ir {

namespace {
ImportResult decode(std::span<const std::byte> raw, std::string_view source) {
    ImportResult result;
    result.scene = semanticSceneFromBytes(raw);
    // Diagnose only successfully decoded legacy input, independently of filename
    // and compression. The library returns diagnostics; callers decide how to show them.
    if (raw.size() >= 8 && std::memcmp(raw.data() + 4, "NHS8", 4) == 0) {
        result.diags.warn(codes::kWarnImportLegacyNhb,
                          "Loaded legacy NHS8 geometry; it remains supported. "
                          "Use 'nodehammer upgrade -i old.nhb.zst -o new.nhb.zst' "
                          "to migrate a standalone geometry to NHS9. "
                          "For .nhproj input, this warning refers to the embedded geometry.",
                          source);
    }
    return result;
}
} // namespace

std::string_view FlatBufferImporter::formatName() const noexcept { return "nhb"; }

std::vector<std::string> FlatBufferImporter::supportedExtensions() const {
    return {"nhb", "nhb.zst"};
}

ImportResult FlatBufferImporter::import(const std::filesystem::path &path) const {
    ImportResult result;

    try {
        auto raw = detail::zstd_io::readBytesFromFile(path);
        result = decode(raw, path.string());
    } catch (const Error &) {
        throw;
    } catch (const std::exception &ex) {
        // The codec and the file reader both throw, and neither type is part of
        // any contract. There is no scene to hand back, so this is the fatal
        // channel — see docs/error-model.md.
        throw Error{codes::kFatalImportFileNotFound,
                    std::format("failed to load FlatBuffer '{}': {}", path.string(), ex.what()),
                    path.string()};
    }

    return result;
}

ImportResult FlatBufferImporter::importFromBytes(std::string_view filename,
                                                 std::span<const std::byte> bytes) {
    ImportResult result;

    try {
        // Byte callers need no filename hint; retain the extension hint so
        // malformed inputs explicitly named .zst still report a zstd error.
        const bool zst = detail::zstd_io::isCompressed(bytes) ||
                         detail::zstd_io::hasZstdExtension(std::filesystem::path{filename});
        std::vector<std::byte> decompressed;
        std::span<const std::byte> raw = bytes;
        if (zst) {
            decompressed = detail::zstd_io::decompress(bytes);
            raw = std::span<const std::byte>{decompressed};
        }
        result = decode(raw, filename);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &ex) {
        throw Error{codes::kFatalImportFileNotFound,
                    std::format("failed to load FlatBuffer '{}': {}", filename, ex.what()),
                    std::string{filename}};
    }

    return result;
}

} // namespace nodehammer::ir
