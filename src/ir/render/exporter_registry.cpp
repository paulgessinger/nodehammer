#include <ir/gltf/render/exporter.hpp>
#include <ir/obj/render/exporter.hpp>
#include <ir/render/exporter.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace nodehammer {

namespace {

std::string toLower(std::string_view s) {
    std::string out{s};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

void RenderExporterRegistry::registerExporter(std::unique_ptr<IRenderExporter> exporter) {
    exporters_.push_back(std::move(exporter));
}

const IRenderExporter *
RenderExporterRegistry::findByFormat(std::string_view formatName) const noexcept {
    for (const auto &exp : exporters_) {
        if (exp->formatName() == formatName) {
            return exp.get();
        }
    }
    return nullptr;
}

const IRenderExporter *
RenderExporterRegistry::findByExtension(std::string_view ext) const noexcept {
    const std::string needle = toLower(ext);
    for (const auto &exp : exporters_) {
        for (const auto &e : exp->supportedExtensions()) {
            if (toLower(e) == needle) {
                return exp.get();
            }
        }
    }
    return nullptr;
}

const IRenderExporter *RenderExporterRegistry::resolve(const std::filesystem::path &path,
                                                       std::string_view formatHint) const noexcept {
    if (!formatHint.empty()) {
        return findByFormat(formatHint);
    }
    const std::string ext = path.extension().string();
    if (ext.size() > 1) {
        return findByExtension(ext);
    }
    return nullptr;
}

RenderExporterRegistry RenderExporterRegistry::makeDefault() {
    RenderExporterRegistry reg;
    reg.registerExporter(std::make_unique<GltfExporter>());
    reg.registerExporter(std::make_unique<ObjExporter>());
    return reg;
}

} // namespace nodehammer
