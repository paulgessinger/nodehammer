#include <api/handles_config.hpp>
#include <api/handles_render.hpp>
#include <nodehammer/io.hpp>
#include <nodehammer/nhr.hpp>

#include <detail/zstd_io.hpp>
#include <diagnostic_codes.hpp>
#include <export_resolve.hpp>
#include <ir/fb/render/flatbuffer.hpp>
#include <ir/render/exporter.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <format>
#include <string>
#include <utility>

namespace nodehammer {
namespace {

void appendUnique(std::vector<std::string> &out, std::string_view name) {
    if (std::find(out.begin(), out.end(), name) == out.end()) {
        out.emplace_back(name);
    }
}

} // namespace

RenderScene readRender(const std::filesystem::path &path) {
    try {
        const auto bytes = detail::zstd_io::readBytesFromFile(path);
        return api::asHandle(ir::renderSceneFromBytes(bytes));
    } catch (const std::exception &e) {
        // `renderSceneFromBytes` throws on a failed verify and the reader throws
        // on a missing file; both are input this call cannot act on, and neither
        // internal type is part of the contract.
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, path.string());
    }
}

RenderScene fromNhr(std::span<const std::byte> nhr) {
    try {
        if (detail::zstd_io::isCompressed(nhr)) {
            return api::asHandle(ir::renderSceneFromBytes(detail::zstd_io::decompress(nhr)));
        }
        return api::asHandle(ir::renderSceneFromBytes(nhr));
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, "<memory>.nhr");
    }
}

std::span<const std::string_view> renderReadFormats() {
    static constexpr std::string_view formats[]{"nhr"};
    return formats;
}

std::span<const std::string_view> renderWriteFormats() {
    static const std::vector<std::string> owned = [] {
        // Straight off the registry, in registration order. `nhr` used to be
        // prepended here by hand, because it was dispatched by `write` rather
        // than registered -- which is exactly what made this list and the CLI's
        // disagree. It is an exporter now, so there is one source.
        std::vector<std::string> out;
        const auto registry = ir::RenderExporterRegistry::makeDefault();
        for (const auto &exp : registry.exporters()) {
            appendUnique(out, exp->formatName());
        }
        return out;
    }();
    static const std::vector<std::string_view> views{owned.begin(), owned.end()};
    return views;
}

void write(const RenderScene &handle, const std::filesystem::path &path, const OutputConfig &output,
           const RenderWriteOptions &options) {
    const auto &scene = api::sceneOrThrow(handle, "write");

    const auto registry = ir::RenderExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        throw Error{
            codes::kFatalExportWriteFailed,
            options.format.empty()
                ? std::format("no exporter claims the extension of '{}'", path.string())
                : std::format("no exporter named '{}' in this build; see renderWriteFormats()",
                              options.format),
            path.string()};
    }

    // The same resolution the CLI runs, from the same function: format
    // defaults, then the matching `[export.<fmt>]` table field by field, then
    // GLB's table-level fallback to `[export.gltf]` (#41 §3).
    auto resolved = pipeline::resolveExportConfig(api::documentOf(output), path, options.format);
    resolved.compressionLevel = options.compressionLevel;
    const auto extensions = exporter->supportedExtensions();
    if (detail::zstd_io::hasZstdExtension(path) &&
        std::none_of(extensions.begin(), extensions.end(),
                     [](const auto &extension) { return extension.ends_with(".zst"); })) {
        throw Error{codes::kFatalExportWriteFailed,
                    "zstd compression is not supported for this output format", path.string()};
    }
    try {
        exporter->write(scene, path, resolved);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed, path.string());
    }
}

std::vector<std::byte> toNhr(const RenderScene &handle) {
    const auto &scene = api::sceneOrThrow(handle, "toNhr");
    try {
        return ir::renderSceneToBytes(scene);
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed);
    }
}

bool RenderScene::valid() const noexcept { return impl_ != nullptr; }

// Members, so they read the state directly — see the note in semantic_scene.cpp.

std::size_t RenderScene::nodeCount() const noexcept {
    return impl_ ? impl_->scene.nodes.size() : 0;
}

std::size_t RenderScene::meshCount() const noexcept {
    return impl_ ? impl_->scene.meshAssets.size() : 0;
}

std::size_t RenderScene::materialCount() const noexcept {
    return impl_ ? impl_->scene.materials.size() : 0;
}

std::size_t RenderScene::triangleCount() const noexcept {
    if (!impl_) {
        return 0;
    }
    std::size_t total = 0;
    for (const auto &[id, mesh] : impl_->scene.meshAssets) {
        total += mesh.indices.size() / 3;
    }
    return total;
}

} // namespace nodehammer
