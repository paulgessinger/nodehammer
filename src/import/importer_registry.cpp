#include <nodehammer/import/importer_registry.hpp>
#include <nodehammer/import/json_importer.hpp>
#include <nodehammer/import/synthetic.hpp>

#ifdef NH_WITH_TGEO
#include <nodehammer/import/tgeo/tgeo_importer.hpp>
#endif
#ifdef NH_WITH_DD4HEP
#include <nodehammer/import/dd4hep/dd4hep_importer.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>

namespace nodehammer {

namespace {

std::string toLower(std::string_view s) {
    std::string out{s};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Read the first 512 bytes of an XML file and return a format name if the
/// root element is recognisable, or an empty string if not.
std::string sniffXmlFormat(const std::filesystem::path &path) {
    std::ifstream f{path, std::ios::binary};
    if (!f) {
        return {};
    }
    std::string head(512, '\0');
    f.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<std::size_t>(f.gcount()));

    if (head.find("<lccdd") != std::string::npos) {
        return "dd4hep";
    }
    return {};
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
        const std::string_view extSv{ext};
        if (const IImporter *imp = findByExtension(extSv.substr(1))) {
            return imp;
        }
        // Extension is ambiguous (e.g. .xml): try content sniffing.
        if (toLower(ext) == ".xml") {
            const std::string sniffed = sniffXmlFormat(path);
            if (!sniffed.empty()) {
                return findByFormat(sniffed);
            }
        }
    }
    return nullptr;
}

const std::vector<std::unique_ptr<IImporter>> &ImporterRegistry::importers() const noexcept {
    return importers_;
}

ImporterRegistry makeDefaultRegistry() {
    ImporterRegistry reg;
    reg.registerImporter(std::make_unique<SyntheticImporter>());
    reg.registerImporter(std::make_unique<JsonImporter>());
#ifdef NH_WITH_TGEO
    reg.registerImporter(std::make_unique<TGeoImporter>());
#endif
#ifdef NH_WITH_DD4HEP
    reg.registerImporter(std::make_unique<DD4hepImporter>());
#endif
    return reg;
}

} // namespace nodehammer
