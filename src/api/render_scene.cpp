#include <api/handles.hpp>

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

RenderScene RenderScene::read(const std::filesystem::path &path) {
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

RenderScene RenderScene::read(std::span<const std::byte> nhr) {
    try {
        return api::asHandle(ir::renderSceneFromBytes(nhr));
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalImportFileNotFound, "<memory>.nhr");
    }
}

std::span<const std::string_view> RenderScene::formats() {
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

void RenderScene::write(const std::filesystem::path &path, const OutputConfig &output,
                        const WriteOptions &options) const {
    const auto &scene = api::sceneOrThrow(*this, "RenderScene::write");

    const auto registry = ir::RenderExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        throw Error{codes::kFatalExportWriteFailed,
                    options.format.empty()
                        ? std::format("no exporter claims the extension of '{}'", path.string())
                        : std::format("no exporter named '{}' in this build; see formats()",
                                      options.format),
                    path.string()};
    }

    // The same resolution the CLI runs, from the same function: format
    // defaults, then the matching `[export.<fmt>]` table field by field, then
    // GLB's table-level fallback to `[export.gltf]` (#41 §3).
    const auto resolved =
        pipeline::resolveExportConfig(api::documentOf(output), path, options.format);
    try {
        exporter->write(scene, path, resolved);
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrowAsError(e, codes::kFatalExportWriteFailed, path.string());
    }
}

std::vector<std::byte> RenderScene::toNhr() const {
    const auto &scene = api::sceneOrThrow(*this, "RenderScene::toNhr");
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
