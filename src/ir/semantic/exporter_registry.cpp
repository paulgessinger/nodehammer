#include <ir/semantic/exporter.hpp>

#include <ir/fb/semantic/exporter.hpp>
#include <ir/json/semantic/exporter.hpp>

#include <algorithm>
#include <cctype>
#include <span>
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

void SemanticExporterRegistry::registerExporter(std::unique_ptr<ISemanticExporter> exporter) {
    exporters_.push_back(std::move(exporter));
}

const ISemanticExporter *
SemanticExporterRegistry::findByFormat(std::string_view formatName) const noexcept {
    for (const auto &exp : exporters_) {
        if (exp->formatName() == formatName) {
            return exp.get();
        }
    }
    return nullptr;
}

const ISemanticExporter *
SemanticExporterRegistry::findByExtension(std::string_view ext) const noexcept {
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

const ISemanticExporter *
SemanticExporterRegistry::resolve(const std::filesystem::path &path,
                                  std::string_view formatHint) const noexcept {
    if (!formatHint.empty()) {
        return findByFormat(formatHint);
    }

    // Try compound extension first (e.g. ".json.zst" -> "json.zst")
    const std::string stem = path.stem().string();
    const std::string ext = path.extension().string();
    if (ext.size() > 1) {
        const auto stemExt = std::filesystem::path{stem}.extension().string();
        if (!stemExt.empty()) {
            const std::string compound = stemExt.substr(1) + ext;
            if (const ISemanticExporter *exp = findByExtension(compound)) {
                return exp;
            }
        }

        const std::string_view extSv{ext};
        if (const ISemanticExporter *exp = findByExtension(extSv.substr(1))) {
            return exp;
        }
    }
    return nullptr;
}

std::span<const std::unique_ptr<ISemanticExporter>>
SemanticExporterRegistry::exporters() const noexcept {
    return exporters_;
}

SemanticExporterRegistry SemanticExporterRegistry::makeDefault() {
    SemanticExporterRegistry reg;
    reg.registerExporter(std::make_unique<SemanticJsonExporter>());
    reg.registerExporter(std::make_unique<SemanticFlatbufferExporter>());
    return reg;
}

} // namespace nodehammer
