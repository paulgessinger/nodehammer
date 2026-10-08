#include <detail/zstd_io.hpp>
#include <ir/expanded/adapt.hpp>
#include <ir/expanded/conversion.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/legacy/nhs8.hpp>
#include <ir/semantic/flatbuffer.hpp>

namespace nodehammer::ir::semantic {
// This stack stage keeps the existing NHS8 wire format. The next PR replaces
// these boundary adapters with the canonical NHS9 codec.
std::vector<std::byte> sceneToBytes(const Scene &scene) {
    return legacy::nhs8::semanticSceneToBytes(expand(scene));
}
Scene sceneFromBytes(std::span<const std::byte> bytes) {
    if (detail::zstd_io::isCompressed(bytes)) {
        const auto raw = detail::zstd_io::decompress(bytes);
        return sceneFromBytes(raw);
    }
    auto scene = legacy::nhs8::semanticSceneFromBytes(bytes);
    scene.computeWorldTransforms();
    scene.computeOriginalPaths();
    return fromExpanded(scene);
}
void writeFlatbuffer(const Scene &scene, const std::filesystem::path &path, int compressionLevel) {
    detail::zstd_io::writeBytesToFile(path, sceneToBytes(scene), compressionLevel);
}
Scene readFlatbuffer(const std::filesystem::path &path) {
    return sceneFromBytes(detail::zstd_io::readBytesFromFile(path));
}
} // namespace nodehammer::ir::semantic

namespace nodehammer::ir {
std::vector<std::byte> semanticSceneToBytes(const semantic::Scene &scene) {
    return semantic::sceneToBytes(scene);
}
semantic::Scene semanticSceneFromBytes(std::span<const std::byte> bytes) {
    return semantic::sceneFromBytes(bytes);
}
} // namespace nodehammer::ir
