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

constexpr std::string_view kNhr = "nhr";

[[nodiscard]] std::string lowerExtension(const std::filesystem::path &path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/// `.nhr` and `.nhr.zst`. The compound form needs the second extension checked
/// explicitly, since `path.extension()` only ever reports the last one.
[[nodiscard]] bool isNhrPath(const std::filesystem::path &path) {
    const auto ext = lowerExtension(path);
    if (ext == ".nhr") {
        return true;
    }
    return ext == ".zst" && lowerExtension(path.stem()) == ".nhr";
}

void appendUnique(std::vector<std::string> &out, std::string_view name) {
    if (std::find(out.begin(), out.end(), name) == out.end()) {
        out.emplace_back(name);
    }
}

} // namespace

RenderResult RenderScene::read(const std::filesystem::path &path) {
    try {
        const auto bytes = detail::zstd_io::readBytesFromFile(path);
        return RenderResult{api::wrap(ir::renderSceneFromBytes(bytes)), DiagnosticList{}};
    } catch (const std::exception &e) {
        // `renderSceneFromBytes` throws on a failed verify and the reader throws
        // on a missing file; both are input this call cannot act on, and neither
        // internal type is part of the contract.
        api::rethrow(e, codes::kErrImportFileNotFound, path.string());
    }
}

RenderResult RenderScene::read(std::span<const std::byte> nhr) {
    try {
        return RenderResult{api::wrap(ir::renderSceneFromBytes(nhr)), DiagnosticList{}};
    } catch (const std::exception &e) {
        api::rethrow(e, codes::kErrImportFileNotFound, "<memory>.nhr");
    }
}

std::span<const std::string_view> RenderScene::formats() {
    static const std::vector<std::string> owned = [] {
        // `nhr` first because it is the only format this type can also *read*;
        // the exporters follow in registration order.
        std::vector<std::string> out{std::string{kNhr}};
        const auto registry = ir::RenderExporterRegistry::makeDefault();
        for (const auto &exp : registry.exporters()) {
            appendUnique(out, exp->formatName());
        }
        return out;
    }();
    static const std::vector<std::string_view> views{owned.begin(), owned.end()};
    return views;
}

DiagnosticList RenderScene::write(const std::filesystem::path &path, const OutputConfig &output,
                                  const WriteOptions &options) const {
    const auto &scene = api::require(*this, "RenderScene::write");

    // The render IR's own format is not in the exporter registry — nothing in
    // the CLI writes one — so it is dispatched here, ahead of the registry that
    // would otherwise report it as unknown.
    if (options.format == kNhr || (options.format.empty() && isNhrPath(path))) {
        try {
            const auto bytes = ir::renderSceneToBytes(scene);
            detail::zstd_io::writeBytesToFile(path, bytes);
            return DiagnosticList{};
        } catch (const std::exception &e) {
            api::rethrow(e, codes::kErrExportWriteFailed, path.string());
        }
    }

    const auto registry = ir::RenderExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        throw Error{codes::kErrExportWriteFailed,
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
        pipeline::resolveExportConfig(api::configOf(output), path, options.format);
    try {
        auto result = exporter->write(scene, path, resolved);
        if (result.diags.hasErrors()) {
            api::throwReported(result.diags, codes::kErrExportWriteFailed, path.string());
        }
        return api::wrap(std::move(result.diags));
    } catch (const Error &) {
        throw;
    } catch (const std::exception &e) {
        api::rethrow(e, codes::kErrExportWriteFailed, path.string());
    }
}

std::vector<std::byte> RenderScene::toNhr() const {
    const auto &scene = api::require(*this, "RenderScene::toNhr");
    try {
        return ir::renderSceneToBytes(scene);
    } catch (const std::exception &e) {
        api::rethrow(e, codes::kErrExportWriteFailed);
    }
}

bool RenderScene::valid() const noexcept { return impl_ != nullptr; }

std::size_t RenderScene::nodeCount() const noexcept {
    const auto *scene = api::sceneOf(*this);
    return scene != nullptr ? scene->nodes.size() : 0;
}

std::size_t RenderScene::meshCount() const noexcept {
    const auto *scene = api::sceneOf(*this);
    return scene != nullptr ? scene->meshAssets.size() : 0;
}

std::size_t RenderScene::materialCount() const noexcept {
    const auto *scene = api::sceneOf(*this);
    return scene != nullptr ? scene->materials.size() : 0;
}

std::size_t RenderScene::triangleCount() const noexcept {
    const auto *scene = api::sceneOf(*this);
    if (scene == nullptr) {
        return 0;
    }
    std::size_t total = 0;
    for (const auto &[id, mesh] : scene->meshAssets) {
        total += mesh.indices.size() / 3;
    }
    return total;
}

} // namespace nodehammer
