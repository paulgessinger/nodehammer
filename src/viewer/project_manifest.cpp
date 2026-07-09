#include <nodehammer/viewer/project_manifest.hpp>

#include <toml++/toml.hpp>

#include <exception>
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

} // namespace nodehammer::viewer
