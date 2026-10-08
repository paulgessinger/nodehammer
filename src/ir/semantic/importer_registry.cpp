#include <ir/fb/semantic/importer.hpp>
#include <ir/json/semantic/importer.hpp>
#include <ir/semantic/importer.hpp>
#include <ir/synthetic/semantic/importer.hpp>

#ifdef NH_WITH_TGEO
#include <ir/tgeo/semantic/importer.hpp>
#endif
#ifdef NH_WITH_DD4HEP
#include <ir/dd4hep/semantic/importer.hpp>

#endif

#include <algorithm>
#include <cctype>
#include <diagnostic_codes.hpp>
#include <format>
#include <fstream>
#include <string>

namespace nodehammer::ir {

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

void ImporterRegistry::registerImporter(std::unique_ptr<ISemanticImporter> importer) {
    importers_.push_back(std::move(importer));
}

const ISemanticImporter *
ImporterRegistry::findByFormat(std::string_view formatName) const noexcept {
    for (const auto &imp : importers_) {
        if (imp->formatName() == formatName) {
            return imp.get();
        }
    }
    return nullptr;
}

const ISemanticImporter *ImporterRegistry::findByExtension(std::string_view ext) const noexcept {
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

const ISemanticImporter *ImporterRegistry::resolveUnchecked(const std::filesystem::path &path,
                                                            std::string_view formatName) const {
    if (!formatName.empty()) {
        return findByFormat(formatName);
    }

    // Try compound extension first (e.g. ".json.zst" → "json.zst")
    const std::string stem = path.stem().string();
    const std::string ext = path.extension().string();
    if (ext.size() > 1) {
        const auto stemExt = std::filesystem::path{stem}.extension().string();
        if (!stemExt.empty()) {
            // Compound: e.g. "foo.json.zst" → stemExt=".json", ext=".zst" → "json.zst"
            const std::string compound = stemExt.substr(1) + ext;
            if (const ISemanticImporter *imp = findByExtension(compound)) {
                return imp;
            }
        }

        const std::string_view extSv{ext};
        if (const ISemanticImporter *imp = findByExtension(extSv.substr(1))) {
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

const std::vector<std::unique_ptr<ISemanticImporter>> &
ImporterRegistry::importers() const noexcept {
    return importers_;
}

const ISemanticImporter *ImporterRegistry::resolve(const std::filesystem::path &path,
                                                   std::string_view formatName) const {
    const auto *importer = resolveUnchecked(path, formatName);
    if (importer) {
        for (const auto &[name, value] : options_) {
            const auto specs = importer->optionSpecs();
            if (std::ranges::none_of(specs, [&](const auto &spec) { return spec.name == name; })) {
                throw Error{codes::kFatalImportOptionsMismatch,
                            std::format("option '{}' does not apply to importer '{}'", name,
                                        importer->formatName()),
                            path.string()};
            }
        }
    }
    return importer;
}

void ImporterRegistry::configure(const ImporterOptions &options) {
    // Validate everything first so a typo or wrong type cannot partially apply options.
    for (const auto &[name, value] : options) {
        const ImporterOptionSpec *found = nullptr;
        for (const auto &importer : importers_) {
            for (const auto &spec : importer->optionSpecs()) {
                if (spec.name == name)
                    found = &spec;
            }
        }
        if (!found) {
            throw Error{
                codes::kFatalImportOptionsMismatch,
                std::format(
                    "unknown importer option '{}' (its backend may be unavailable in this build)",
                    name)};
        }
        if (found->defaultValue.index() != value.index()) {
            static constexpr std::string_view types[]{"bool", "int64", "double", "string"};
            throw Error{codes::kFatalImportOptionsMismatch,
                        std::format("option '{}' expects {}, got {}", name,
                                    types[found->defaultValue.index()], types[value.index()])};
        }
    }
    for (auto &importer : importers_) {
        ImporterOptions effective;
        for (const auto &spec : importer->optionSpecs()) {
            const auto it = options.find(spec.name);
            effective.emplace(spec.name, it == options.end() ? spec.defaultValue : it->second);
        }
        importer->configure(effective);
    }
    options_ = options;
}

void ImporterRegistry::setOption(std::string name, ImporterOptionValue value) {
    auto options = options_;
    options.insert_or_assign(std::move(name), std::move(value));
    configure(options);
}

void ImporterRegistry::decorate(std::string_view format, const Decorator &decorator) {
    for (auto &importer : importers_) {
        if (importer->formatName() == format) {
            importer = decorator(std::move(importer));
            configure(options_);
            return;
        }
    }
}

ImporterRegistry ImporterRegistry::makeDefault(const SemanticReadOptions &options) {
    ImporterRegistry reg;
    reg.registerImporter(std::make_unique<SyntheticImporter>());
    reg.registerImporter(std::make_unique<JsonImporter>());
    reg.registerImporter(std::make_unique<FlatBufferImporter>());
#ifdef NH_WITH_TGEO
    reg.registerImporter(std::make_unique<TGeoImporter>());
#endif
#ifdef NH_WITH_DD4HEP
    reg.registerImporter(std::make_unique<DD4hepImporter>());
#endif
    reg.configure(options.importerOptions);
    return reg;
}

} // namespace nodehammer::ir
