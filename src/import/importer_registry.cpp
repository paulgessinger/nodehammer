#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/import/synthetic.hpp>

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

void ImporterRegistry::registerImporter(std::unique_ptr<IImporter> importer) {
    importers_.push_back(std::move(importer));
}

const IImporter *ImporterRegistry::findByFormat(std::string_view formatName) const noexcept {
    for (const auto &imp : importers_) {
        if (imp->formatName() == formatName) {
            return imp.get();
        }
    }
    return nullptr;
}

const IImporter *ImporterRegistry::findByExtension(std::string_view ext) const noexcept {
    const std::string needle = toLower(ext);
    for (const auto &imp : importers_) {
        for (const auto &e : imp->supportedExtensions()) {
            if (toLower(e) == needle) {
                return imp.get();
            }
        }
    }
    return nullptr;
}

const IImporter *ImporterRegistry::resolve(const std::filesystem::path &path,
                                           std::string_view formatName) const noexcept {
    if (!formatName.empty()) {
        return findByFormat(formatName);
    }
    const std::string ext = path.extension().string();
    if (ext.size() > 1) {
        return findByExtension(std::string_view{ext}.substr(1)); // strip leading '.'
    }
    return nullptr;
}

const std::vector<std::unique_ptr<IImporter>> &ImporterRegistry::importers() const noexcept {
    return importers_;
}

ImporterRegistry makeDefaultRegistry() {
    ImporterRegistry reg;
    reg.registerImporter(std::make_unique<SyntheticImporter>());
    return reg;
}

} // namespace nodehammer
