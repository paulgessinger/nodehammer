#include <tessellation/shading.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <vector>

namespace nodehammer::tessellation {

void applyShading(TessellationOutput &mesh, config::Shading shading, double smoothAngle) {
    if (shading == config::Shading::Preserve || mesh.indices.empty())
        return;

    const size_t count = mesh.indices.size();
    std::vector<glm::dvec3> normals(count / 3, glm::dvec3{0});
    std::vector<double> weights(count, 0);
    std::vector<size_t> parent(count);
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](size_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    auto position = [&](size_t corner) {
        return glm::dvec3(mesh.vertices[mesh.indices[corner]].position);
    };

    // Weld only exact duplicate positions for adjacency, never move vertices.
    // This recovers topology from triangle soups produced by boolean/import paths.
    std::map<std::array<double, 3>, size_t> positions;
    std::vector<size_t> ids(count);
    struct EdgeUse {
        size_t a, b;
    };
    std::map<std::pair<size_t, size_t>, std::vector<EdgeUse>> edges;
    for (size_t t = 0; t < count; t += 3) {
        const auto cross = glm::cross(position(t + 1) - position(t), position(t + 2) - position(t));
        const double length = glm::length(cross);
        if (!std::isfinite(length) || length == 0)
            continue;
        normals[t / 3] = cross / length;
        for (size_t j = 0; j < 3; ++j) {
            const size_t c = t + j;
            const auto p = position(c);
            const auto a = glm::normalize(position(t + (j + 1) % 3) - p);
            const auto b = glm::normalize(position(t + (j + 2) % 3) - p);
            weights[c] = std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
            if (shading == config::Shading::Smooth) {
                auto [it, inserted] = positions.try_emplace({p.x, p.y, p.z}, positions.size());
                ids[c] = it->second;
            }
        }
        if (shading == config::Shading::Smooth) {
            for (size_t j = 0; j < 3; ++j) {
                size_t a = t + j, b = t + (j + 1) % 3;
                edges[std::minmax(ids[a], ids[b])].push_back({a, b});
            }
        }
    }
    if (shading == config::Shading::Smooth) {
        const double threshold = std::cos(glm::radians(smoothAngle));
        for (const auto &[key, uses] : edges) {
            if (uses.size() != 2)
                continue;
            const auto [a, b] = uses[0];
            const auto [c, d] = uses[1];
            // Opposite winding is required: touching shells or duplicate faces
            // must not be joined just because they contain the same edge.
            if (ids[a] != ids[d] || ids[b] != ids[c])
                continue;
            if (glm::dot(normals[a / 3], normals[c / 3]) + 1e-12 < threshold)
                continue;
            parent[root(a)] = root(d);
            parent[root(b)] = root(c);
        }
    }
    std::vector<glm::dvec3> sums(count, glm::dvec3{0});
    for (size_t c = 0; c < count; ++c)
        sums[root(c)] += normals[c / 3] * weights[c];

    // Share vertices inside a smooth fan; split corners at creases. Flat mode
    // deliberately leaves every corner independent.
    std::vector<ir::render::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<size_t> outputIndex(count, count);
    vertices.reserve(count);
    indices.reserve(count);
    for (size_t c = 0; c < count; ++c) {
        const auto r = root(c);
        if (outputIndex[r] == count) {
            auto vertex = mesh.vertices[mesh.indices[c]];
            const auto n = sums[r];
            const double length = glm::length(n);
            vertex.normal = length > 1e-15 ? glm::vec3(n / length) : glm::vec3(normals[c / 3]);
            outputIndex[r] = vertices.size();
            vertices.push_back(vertex);
        }
        indices.push_back(static_cast<uint32_t>(outputIndex[r]));
    }
    mesh.vertices = std::move(vertices);
    mesh.indices = std::move(indices);
}

} // namespace nodehammer::tessellation
