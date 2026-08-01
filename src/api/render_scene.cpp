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

[[nodiscard]] RenderResult failure(std::string_view code, std::string_view message,
                                   std::string_view context = {}) {
    return RenderResult{RenderScene{}, api::error(code, message, context)};
}

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
        // renderSceneFromBytes throws on a failed verify, and the reader throws
        // on a missing file. Neither may cross the API (#41 principle 5).
        return failure(codes::kErrImportFileNotFound, e.what(), path.string());
    }
}

RenderResult RenderScene::read(std::span<const std::byte> nhr) {
    try {
        return RenderResult{api::wrap(ir::renderSceneFromBytes(nhr)), DiagnosticList{}};
    } catch (const std::exception &e) {
        return failure(codes::kErrImportFileNotFound, e.what());
    }
}

std::vector<std::string> RenderScene::formats() {
    // `nhr` first because it is the only format this type can also *read*; the
    // exporters follow in registration order.
    std::vector<std::string> out{std::string{kNhr}};
    const auto registry = ir::RenderExporterRegistry::makeDefault();
    for (const auto &exp : registry.exporters()) {
        appendUnique(out, exp->formatName());
    }
    return out;
}

DiagnosticList RenderScene::write(const std::filesystem::path &path, const OutputConfig &output,
                                  const WriteOptions &options) const {
    const auto *scene = api::sceneOf(*this);
    if (scene == nullptr) {
        return api::error(codes::kErrApiInvalidHandle, "cannot write an empty scene handle",
                          path.string());
    }

    // The render IR's own format is not in the exporter registry — nothing in
    // the CLI writes one — so it is dispatched here, ahead of the registry that
    // would otherwise report it as unknown.
    if (options.format == kNhr || (options.format.empty() && isNhrPath(path))) {
        try {
            const auto bytes = ir::renderSceneToBytes(*scene);
            detail::zstd_io::writeBytesToFile(path, bytes);
            return DiagnosticList{};
        } catch (const std::exception &e) {
            return api::error(codes::kErrExportWriteFailed, e.what(), path.string());
        }
    }

    const auto registry = ir::RenderExporterRegistry::makeDefault();
    const auto *exporter = registry.resolve(path, options.format);
    if (exporter == nullptr) {
        return api::error(
            codes::kErrExportWriteFailed,
            options.format.empty()
                ? std::format("no exporter claims the extension of '{}'", path.string())
                : std::format("no exporter named '{}' in this build; see formats()",
                              options.format),
            path.string());
    }

    // The same resolution the CLI runs, from the same function: format
    // defaults, then the matching `[export.<fmt>]` table field by field, then
    // GLB's table-level fallback to `[export.gltf]` (#41 §3).
    const auto resolved =
        pipeline::resolveExportConfig(api::configOf(output), path, options.format);
    try {
        return api::wrap(exporter->write(*scene, path, resolved).diags);
    } catch (const std::exception &e) {
        return api::error(codes::kErrExportWriteFailed, e.what(), path.string());
    }
}

std::vector<std::byte> RenderScene::toNhr() const {
    const auto *scene = api::sceneOf(*this);
    if (scene == nullptr) {
        return {};
    }
    try {
        return ir::renderSceneToBytes(*scene);
    } catch (const std::exception &) {
        return {};
    }
}

bool RenderScene::valid() const noexcept { return api::sceneOf(*this) != nullptr; }

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
