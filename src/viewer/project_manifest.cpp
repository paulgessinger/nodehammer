#include <viewer/project_manifest.hpp>

#include <toml++/toml.hpp>

#include <exception>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace nodehammer::viewer {

std::optional<ProjectManifest> parseProjectManifest(std::span<const std::byte> toml_bytes) {
    try {
        auto tbl = toml::parse(
            std::string_view{reinterpret_cast<const char *>(toml_bytes.data()), toml_bytes.size()});
        const auto proj = tbl["project"];
        ProjectManifest m;
        m.config_key = proj["config"].value_or(std::string{});
        m.geometry_key = proj["geometry"].value_or(std::string{});
        if (m.config_key.empty() || m.geometry_key.empty()) {
            return std::nullopt;
        }
        return m;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

std::string serializeProjectManifest(const ProjectManifest &manifest) {
    // Built through toml++ rather than formatted by hand: the keys are file
    // paths, so quoting/escaping is the library's problem, not ours.
    toml::table project;
    project.insert("config", manifest.config_key);
    project.insert("geometry", manifest.geometry_key);

    toml::table root;
    root.insert("project", std::move(project));

    std::ostringstream out;
    out << "# Project manifest — names the entry config and geometry so this\n"
           "# archive builds on open. See ProjectManifest.\n"
        << root << '\n';
    return out.str();
}

} // namespace nodehammer::viewer
