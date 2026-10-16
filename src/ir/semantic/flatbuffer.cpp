#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <detail/file_io.hpp>
#include <detail/overloaded.hpp>
#include <detail/zstd_io.hpp>
#include <ir/expanded/adapt.hpp>
#include <ir/fb/semantic/flatbuffer.hpp>
#include <ir/legacy/nhs8.hpp>
#include <ir/semantic/flatbuffer.hpp>
#include <limits>
#include <shared_generated.h>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace nodehammer::ir::semantic {
namespace {
namespace fbs = nodehammer::shared_fbs;
struct TransformPoolBuild {
    std::vector<double> values;
    std::map<std::array<uint64_t, 16>, uint32_t> indices;
    uint32_t internTransform(const glm::dmat4 &m) {
        std::array<uint64_t, 16> key{};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                key[static_cast<std::size_t>(c * 4 + r)] = std::bit_cast<uint64_t>(m[c][r]);
        if (auto it = indices.find(key); it != indices.end())
            return it->second;
        if (values.size() / 16 > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("NHS9: too many transforms");
        auto index = static_cast<uint32_t>(values.size() / 16);
        indices.emplace(key, index);
        for (auto v : key)
            values.push_back(std::bit_cast<double>(v));
        return index;
    }
};
glm::dmat4 decodeTransform(const flatbuffers::Vector<double> *values, uint32_t index) {
    const uint64_t offset = uint64_t(index) * 16;
    if (!values || values->size() % 16 || offset + 16 > values->size())
        throw std::runtime_error("NHS9: invalid transform reference");
    glm::dmat4 result{1};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            result[c][r] = values->Get(
                static_cast<flatbuffers::uoffset_t>(offset + static_cast<uint64_t>(c * 4 + r)));
    return result;
}
struct ShapeOffsetResult {
    fbs::ShapeData type;
    flatbuffers::Offset<void> offset;
};

ShapeOffsetResult serializeShapeVariant(flatbuffers::FlatBufferBuilder &builder,
                                        TransformPoolBuild &transformPool,
                                        const semantic::ShapeVariant &data) {
    return std::visit(
        detail::overloaded{
            [&](const semantic::BoxShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateBoxShape(builder, s.dx, s.dy, s.dz);
                return {fbs::ShapeData_BoxShape, o.Union()};
            },
            [&](const semantic::TubeShape &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateTubeShape(builder, s.rMin, s.rMax, s.dz, s.phiStart, s.phiDelta);
                return {fbs::ShapeData_TubeShape, o.Union()};
            },
            [&](const semantic::ConeShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateConeShape(builder, s.rMin1, s.rMax1, s.rMin2, s.rMax2, s.dz,
                                              s.phiStart, s.phiDelta);
                return {fbs::ShapeData_ConeShape, o.Union()};
            },
            [&](const semantic::TrdShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateTrdShape(builder, s.dx1, s.dx2, s.dy1, s.dy2, s.dz);
                return {fbs::ShapeData_TrdShape, o.Union()};
            },
            [&](const semantic::ParaShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateParaShape(builder, s.dx, s.dy, s.dz, s.alpha, s.theta, s.phi);
                return {fbs::ShapeData_ParaShape, o.Union()};
            },
            [&](const semantic::PconShape &s) -> ShapeOffsetResult {
                std::vector<fbs::Section> secs;
                secs.reserve(s.sections.size());
                for (const auto &sec : s.sections) {
                    secs.emplace_back(sec.z, sec.rMin, sec.rMax);
                }
                auto o = fbs::CreatePconShape(builder, s.phiStart, s.phiDelta,
                                              builder.CreateVectorOfStructs(secs));
                return {fbs::ShapeData_PconShape, o.Union()};
            },
            [&](const semantic::PgonShape &s) -> ShapeOffsetResult {
                std::vector<fbs::Section> secs;
                secs.reserve(s.sections.size());
                for (const auto &sec : s.sections) {
                    secs.emplace_back(sec.z, sec.rMin, sec.rMax);
                }
                auto o = fbs::CreatePgonShape(builder, s.phiStart, s.phiDelta, s.nSides,
                                              builder.CreateVectorOfStructs(secs));
                return {fbs::ShapeData_PgonShape, o.Union()};
            },
            [&](const semantic::TorusShape &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateTorusShape(builder, s.rMin, s.rMax, s.rTor, s.phiStart, s.phiDelta);
                return {fbs::ShapeData_TorusShape, o.Union()};
            },
            [&](const semantic::ExtrudedShape &s) -> ShapeOffsetResult {
                std::vector<fbs::DVec2> points;
                std::vector<fbs::ExtrudedSection> sections;
                for (const auto &p : s.polygon)
                    points.emplace_back(p.x, p.y);
                for (const auto &sec : s.sections)
                    sections.emplace_back(sec.z, sec.offset.x, sec.offset.y, sec.scale);
                const auto p = builder.CreateVectorOfStructs(points);
                const auto z = builder.CreateVectorOfStructs(sections);
                const auto o = fbs::CreateExtrudedShape(builder, p, z);
                return {fbs::ShapeData_ExtrudedShape, o.Union()};
            },
            [&](const semantic::GenericTrapShape &s) -> ShapeOffsetResult {
                std::vector<fbs::DVec2> points;
                for (const auto &p : s.vertices)
                    points.emplace_back(p.x, p.y);
                const auto o = fbs::CreateGenericTrapShape(builder, s.dz,
                                                           builder.CreateVectorOfStructs(points));
                return {fbs::ShapeData_GenericTrapShape, o.Union()};
            },
            [&](const semantic::TessellatedShape &s) -> ShapeOffsetResult {
                std::vector<fbs::Triangle> tris;
                tris.reserve(s.triangles.size());
                for (const auto &tri : s.triangles) {
                    tris.emplace_back(
                        fbs::DVec3{tri.vertices[0].x, tri.vertices[0].y, tri.vertices[0].z},
                        fbs::DVec3{tri.vertices[1].x, tri.vertices[1].y, tri.vertices[1].z},
                        fbs::DVec3{tri.vertices[2].x, tri.vertices[2].y, tri.vertices[2].z});
                }
                auto o = fbs::CreateTessellatedShape(builder, builder.CreateVectorOfStructs(tris));
                return {fbs::ShapeData_TessellatedShape, o.Union()};
            },
            [&](const semantic::BooleanUnion &s) -> ShapeOffsetResult {
                auto o = fbs::CreateBooleanUnion(builder, s.left.value, s.right.value,
                                                 transformPool.internTransform(s.rightTransform));
                return {fbs::ShapeData_BooleanUnion, o.Union()};
            },
            [&](const semantic::BooleanIntersection &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateBooleanIntersection(builder, s.left.value, s.right.value,
                                                   transformPool.internTransform(s.rightTransform));
                return {fbs::ShapeData_BooleanIntersection, o.Union()};
            },
            [&](const semantic::BooleanSubtraction &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateBooleanSubtraction(builder, s.left.value, s.right.value,
                                                  transformPool.internTransform(s.rightTransform));
                return {fbs::ShapeData_BooleanSubtraction, o.Union()};
            },
            [&](const semantic::UnknownShape &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateUnknownShape(builder, builder.CreateSharedString(s.originalType));
                return {fbs::ShapeData_UnknownShape, o.Union()};
            },
        },
        data);
}

// ── Shape deserialization ───────────────────────────────────────────────────

semantic::ShapeVariant deserializeShapeVariant(fbs::ShapeData type, const void *data,
                                               const flatbuffers::Vector<double> *transforms) {
    if (!data)
        throw std::runtime_error("NHS9: missing shape payload");
    switch (type) {
    case fbs::ShapeData_BoxShape: {
        const auto *s = static_cast<const fbs::BoxShape *>(data);
        return semantic::BoxShape{s->dx(), s->dy(), s->dz()};
    }
    case fbs::ShapeData_TubeShape: {
        const auto *s = static_cast<const fbs::TubeShape *>(data);
        return semantic::TubeShape{s->r_min(), s->r_max(), s->dz(), s->phi_start(), s->phi_delta()};
    }
    case fbs::ShapeData_ConeShape: {
        const auto *s = static_cast<const fbs::ConeShape *>(data);
        return semantic::ConeShape{s->r_min1(), s->r_max1(),    s->r_min2(),   s->r_max2(),
                                   s->dz(),     s->phi_start(), s->phi_delta()};
    }
    case fbs::ShapeData_TrdShape: {
        const auto *s = static_cast<const fbs::TrdShape *>(data);
        return semantic::TrdShape{s->dx1(), s->dx2(), s->dy1(), s->dy2(), s->dz()};
    }
    case fbs::ShapeData_ParaShape: {
        const auto *s = static_cast<const fbs::ParaShape *>(data);
        return semantic::ParaShape{s->dx(), s->dy(), s->dz(), s->alpha(), s->theta(), s->phi()};
    }
    case fbs::ShapeData_PconShape: {
        const auto *s = static_cast<const fbs::PconShape *>(data);
        semantic::PconShape result;
        result.phiStart = s->phi_start();
        result.phiDelta = s->phi_delta();
        if (s->sections()) {
            result.sections.reserve(s->sections()->size());
            for (const auto *sec : *s->sections()) {
                result.sections.push_back({sec->z(), sec->r_min(), sec->r_max()});
            }
        }
        return result;
    }
    case fbs::ShapeData_PgonShape: {
        const auto *s = static_cast<const fbs::PgonShape *>(data);
        semantic::PgonShape result;
        result.phiStart = s->phi_start();
        result.phiDelta = s->phi_delta();
        result.nSides = s->n_sides();
        if (s->sections()) {
            result.sections.reserve(s->sections()->size());
            for (const auto *sec : *s->sections()) {
                result.sections.push_back({sec->z(), sec->r_min(), sec->r_max()});
            }
        }
        return result;
    }
    case fbs::ShapeData_TorusShape: {
        const auto *s = static_cast<const fbs::TorusShape *>(data);
        return semantic::TorusShape{s->r_min(), s->r_max(), s->r_tor(), s->phi_start(),
                                    s->phi_delta()};
    }
    case fbs::ShapeData_ExtrudedShape: {
        const auto *s = static_cast<const fbs::ExtrudedShape *>(data);
        semantic::ExtrudedShape result;
        for (const auto *p : *s->polygon())
            result.polygon.push_back({p->x(), p->y()});
        for (const auto *z : *s->sections())
            result.sections.push_back({z->z(), {z->x(), z->y()}, z->scale()});
        return result;
    }
    case fbs::ShapeData_GenericTrapShape: {
        const auto *s = static_cast<const fbs::GenericTrapShape *>(data);
        if (s->vertices()->size() != 8)
            throw std::runtime_error("NHS9: generic trap requires eight vertices");
        semantic::GenericTrapShape result;
        result.dz = s->dz();
        for (flatbuffers::uoffset_t i = 0; i < 8; ++i) {
            const auto *p = s->vertices()->Get(i);
            result.vertices[i] = {p->x(), p->y()};
        }
        return result;
    }
    case fbs::ShapeData_TessellatedShape: {
        const auto *s = static_cast<const fbs::TessellatedShape *>(data);
        semantic::TessellatedShape result;
        if (s->triangles()) {
            result.triangles.reserve(s->triangles()->size());
            for (const auto *tri : *s->triangles()) {
                semantic::TessellatedShape::Triangle t;
                t.vertices[0] = {tri->v0().x(), tri->v0().y(), tri->v0().z()};
                t.vertices[1] = {tri->v1().x(), tri->v1().y(), tri->v1().z()};
                t.vertices[2] = {tri->v2().x(), tri->v2().y(), tri->v2().z()};
                result.triangles.push_back(t);
            }
        }
        return result;
    }
    case fbs::ShapeData_BooleanUnion: {
        const auto *s = static_cast<const fbs::BooleanUnion *>(data);
        semantic::BooleanUnion result;
        result.left = semantic::ShapeId{s->left()};
        result.right = semantic::ShapeId{s->right()};
        result.rightTransform = decodeTransform(transforms, s->right_transform_index());
        return result;
    }
    case fbs::ShapeData_BooleanIntersection: {
        const auto *s = static_cast<const fbs::BooleanIntersection *>(data);
        semantic::BooleanIntersection result;
        result.left = semantic::ShapeId{s->left()};
        result.right = semantic::ShapeId{s->right()};
        result.rightTransform = decodeTransform(transforms, s->right_transform_index());
        return result;
    }
    case fbs::ShapeData_BooleanSubtraction: {
        const auto *s = static_cast<const fbs::BooleanSubtraction *>(data);
        semantic::BooleanSubtraction result;
        result.left = semantic::ShapeId{s->left()};
        result.right = semantic::ShapeId{s->right()};
        result.rightTransform = decodeTransform(transforms, s->right_transform_index());
        return result;
    }
    case fbs::ShapeData_UnknownShape: {
        const auto *s = static_cast<const fbs::UnknownShape *>(data);
        return semantic::UnknownShape{s->original_type() ? s->original_type()->str() : ""};
    }
    default:
        throw std::runtime_error("NHS9: unsupported shape union");
    }
}

std::size_t indexOf(uint64_t value) {
    if (value > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("NHS9: occurrence index exceeds platform range");
    return static_cast<std::size_t>(value);
}
template <class Map, class Key, class Value> void insertUnique(Map &map, Key key, Value value) {
    if (!map.emplace(std::move(key), std::move(value)).second)
        throw std::runtime_error("NHS9: duplicate catalog or metadata key");
}
template <class Map> auto sortedKeys(const Map &map) {
    std::vector<typename Map::key_type> keys;
    keys.reserve(map.size());
    for (const auto &[key, value] : map)
        keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    return keys;
}
// Column lengths and references are checked before they are dereferenced.
uint32_t columnIndex(uint64_t value) {
    if (value > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("NHS9: column exceeds uint32 range");
    return static_cast<uint32_t>(value);
}
// Unsigned delta arithmetic is modulo 2^N, preserving descending references
// and the complete uint64 ID range. Validate decoded references below.
template <class UInt> void deltaEncode(std::vector<UInt> &values) {
    UInt previous = 0;
    for (auto &value : values) {
        const auto current = value;
        value -= previous;
        previous = current;
    }
}
template <class UInt> std::vector<UInt> decodeDeltas(const flatbuffers::Vector<UInt> *deltas) {
    std::vector<UInt> result;
    result.reserve(deltas->size());
    UInt value = 0;
    for (const auto delta : *deltas) {
        value += delta;
        result.push_back(value);
    }
    return result;
}
template <class UInt> struct DeltaColumn {
    std::vector<UInt> values;
    explicit DeltaColumn(const flatbuffers::Vector<UInt> *deltas) : values(decodeDeltas(deltas)) {}
    std::size_t size() const { return values.size(); }
    UInt Get(uint32_t index) const { return values.at(index); }
};
using IndexColumn = DeltaColumn<uint32_t>;
struct StringColumn {
    const std::vector<std::string> &dictionary;
    std::vector<uint32_t> decoded;
    const flatbuffers::Vector<uint32_t> *absolute;
    StringColumn(const flatbuffers::Vector<uint32_t> *indices,
                 const std::vector<std::string> &strings, bool delta)
        : dictionary(strings), decoded(delta ? decodeDeltas(indices) : std::vector<uint32_t>{}),
          absolute(delta ? nullptr : indices) {
        for (uint32_t i = 0; i < size(); ++i)
            if (index(i) >= dictionary.size())
                throw std::runtime_error("NHS9: invalid string reference");
    }
    std::size_t size() const { return absolute ? absolute->size() : decoded.size(); }
    uint32_t index(uint32_t i) const { return absolute ? absolute->Get(i) : decoded.at(i); }
    const std::string &Get(uint32_t i) const { return dictionary.at(index(i)); }
};
// Bound prefix expansion independently of the much smaller encoded suffixes.
// Use subtraction before addition so these checks also hold on 32-bit WASM.
void addDecodedSize(std::size_t &total, std::size_t prefix, std::size_t suffix, std::size_t limit,
                    const char *message) {
    if (prefix > limit || suffix > limit - prefix || prefix + suffix > limit - total)
        throw std::runtime_error(message);
    total += prefix + suffix;
}
// IDs follow deterministic catalog/metadata traversal order. Keep that locality
// for both string prefixes and reference columns; sorting costs more and makes
// the measured direct ODD file larger. Views borrow the immutable input scene.
struct StringPoolBuild {
    std::unordered_map<std::string_view, uint32_t> indices;
    std::vector<std::string_view> values;
    uint32_t intern(const std::string &value) {
        if (const auto it = indices.find(value); it != indices.end())
            return it->second;
        const auto index = columnIndex(values.size());
        indices.emplace(value, index);
        values.push_back(value);
        return index;
    }
    auto encode(flatbuffers::FlatBufferBuilder &b) const {
        std::vector<uint32_t> prefixes, lengths;
        std::vector<uint8_t> bytes;
        std::string_view previous;
        std::size_t total = 0;
        for (const auto value : values) {
            addDecodedSize(total, 0, value.size(), FLATBUFFERS_MAX_BUFFER_SIZE - 1,
                           "NHS9: decoded strings exceed size limit");
            std::size_t prefix = 0;
            while (prefix < previous.size() && prefix < value.size() &&
                   previous[prefix] == value[prefix])
                ++prefix;
            prefixes.push_back(columnIndex(prefix));
            lengths.push_back(columnIndex(value.size() - prefix));
            const auto suffix = value.substr(prefix);
            bytes.insert(bytes.end(), suffix.begin(), suffix.end());
            previous = value;
        }
        return fbs::CreateStringPoolDirect(b, &prefixes, &lengths, &bytes);
    }
};
std::vector<std::string> decodeStrings(const fbs::StringPool *pool) {
    if (pool->prefix_lengths()->size() != pool->suffix_lengths()->size())
        throw std::runtime_error("NHS9: mismatched string columns");
    std::vector<std::string> result;
    result.reserve(pool->prefix_lengths()->size());
    std::size_t offset = 0, total = 0;
    for (uint32_t i = 0; i < pool->prefix_lengths()->size(); ++i) {
        const auto prefix = pool->prefix_lengths()->Get(i);
        const auto length = pool->suffix_lengths()->Get(i);
        if (prefix > (result.empty() ? 0 : result.back().size()) ||
            length > pool->suffix_bytes()->size() - offset)
            throw std::runtime_error("NHS9: invalid string prefix or suffix");
        addDecodedSize(total, prefix, length, FLATBUFFERS_MAX_BUFFER_SIZE - 1,
                       "NHS9: decoded strings exceed size limit");
        std::string value;
        value.reserve(static_cast<std::size_t>(prefix) + length);
        if (prefix)
            value.assign(result.back().data(), prefix);
        value.append(reinterpret_cast<const char *>(pool->suffix_bytes()->Data() + offset), length);
        result.push_back(std::move(value));
        offset += length;
    }
    if (offset != pool->suffix_bytes()->size())
        throw std::runtime_error("NHS9: trailing string suffix bytes");
    return result;
}
template <class Map> auto catalogIndices(const Map &values) {
    std::unordered_map<typename Map::key_type, uint32_t> result;
    result.reserve(values.size());
    std::size_t row = 1;
    for (auto id : sortedKeys(values))
        result.emplace(id, columnIndex(row++));
    return result;
}
struct EncodedColumns {
    flatbuffers::Offset<fbs::VolumeColumns> volumes;
    flatbuffers::Offset<fbs::MetadataColumns> metadata;
    flatbuffers::Offset<fbs::StringPool> strings;
};
EncodedColumns encodeColumns(flatbuffers::FlatBufferBuilder &b, TransformPoolBuild &transforms,
                             const Scene &scene) {
    const auto volumeIndices = catalogIndices(scene.logVols);
    const auto shapeIndices = catalogIndices(scene.shapes);
    const auto materialIndices = catalogIndices(scene.materials);
    StringPoolBuild dictionary;
    std::vector<uint64_t> ids;
    std::vector<uint32_t> names, daughterNames;
    std::vector<uint32_t> shapes, materials, offsets{0}, daughterVolumes, daughterTransforms;
    for (auto id : sortedKeys(scene.logVols)) {
        const auto &v = scene.logVols.at(id);
        ids.push_back(id.value);
        names.push_back(dictionary.intern(v.name));
        shapes.push_back(shapeIndices.at(v.shapeId));
        materials.push_back(materialIndices.at(v.materialId));
        for (const auto &d : v.daughters) {
            daughterNames.push_back(dictionary.intern(d.name));
            daughterVolumes.push_back(volumeIndices.at(d.logVolId));
            daughterTransforms.push_back(transforms.internTransform(d.localTransform));
        }
        offsets.push_back(columnIndex(daughterNames.size()));
    }
    deltaEncode(names);
    deltaEncode(ids);
    deltaEncode(daughterVolumes);
    deltaEncode(offsets);
    const auto vol =
        fbs::CreateVolumeColumnsDirect(b, &ids, &names, &shapes, &materials, &offsets,
                                       &daughterNames, &daughterVolumes, &daughterTransforms);
    const auto &m = scene.metadata;
    std::map<OccurrenceId, uint32_t> pathIndices;
    std::vector<uint32_t> pathOffsets{0};
    std::vector<uint64_t> pathComponents;
    std::vector<uint32_t> pathPrefixes;
    OccurrenceId previousPath;
    std::size_t totalPathComponents = 0;
    auto pathIndex = [&](const OccurrenceId &path) {
        const auto [it, inserted] = pathIndices.emplace(path, columnIndex(pathIndices.size()));
        if (inserted) {
            addDecodedSize(totalPathComponents, 0, path.size(),
                           FLATBUFFERS_MAX_BUFFER_SIZE / sizeof(uint64_t) - 1,
                           "NHS9: decoded paths exceed size limit");
            std::size_t prefix = 0;
            while (prefix < previousPath.size() && prefix < path.size() &&
                   previousPath[prefix] == path[prefix])
                ++prefix;
            pathPrefixes.push_back(columnIndex(prefix));
            pathComponents.insert(pathComponents.end(),
                                  path.begin() + static_cast<OccurrenceId::difference_type>(prefix),
                                  path.end());
            previousPath = path;
            pathOffsets.push_back(columnIndex(pathComponents.size()));
        }
        return it->second;
    };
    auto pairs = [&](const auto &map, auto key) {
        std::vector<uint32_t> keys, values;
        for (const auto &[k, v] : map) {
            keys.push_back(key(k));
            values.push_back(columnIndex(v));
        }
        deltaEncode(keys);
        return fbs::CreateIndexPairsDirect(b, &keys, &values);
    };
    auto strings = [&](const auto &map) {
        std::vector<uint32_t> paths;
        std::vector<uint32_t> values;
        for (const auto &[p, s] : map) {
            paths.push_back(pathIndex(p));
            values.push_back(dictionary.intern(s));
        }
        deltaEncode(paths);
        deltaEncode(values);
        return fbs::CreateStringOverridesDirect(b, &paths, &values);
    };
    std::vector<flatbuffers::Offset<fbs::TagSet>> sets;
    for (const auto &set : m.tagSets) {
        std::vector<flatbuffers::Offset<fbs::Tag>> tags;
        for (const auto &[key, value] : set) {
            const auto k = b.CreateSharedString(key), v = b.CreateSharedString(value);
            tags.push_back(fbs::CreateTag(b, k, v));
        }
        sets.push_back(fbs::CreateTagSet(b, b.CreateVector(tags)));
    }
    const auto vt = pairs(m.volumeTags, [&](auto id) { return volumeIndices.at(id); });
    const auto ot = pairs(m.occurrenceTags, pathIndex);
    const auto dn = strings(m.displayNames), pn = strings(m.pathNames);
    const auto op = strings(m.originalPaths), ss = strings(m.sourceSystems);
    std::vector<uint32_t> placementVolumes, placementSets, degradationPaths;
    std::vector<uint64_t> placementDaughters, degradationFlags;
    for (const auto &[key, set] : m.placementTags) {
        placementVolumes.push_back(volumeIndices.at(key.first));
        placementDaughters.push_back(key.second);
        placementSets.push_back(columnIndex(set));
    }
    for (const auto &[path, flags] : m.degradation) {
        degradationPaths.push_back(pathIndex(path));
        degradationFlags.push_back(flags.bits.to_ullong());
    }
    const auto tagSets = b.CreateVector(sets);
    deltaEncode(pathOffsets);
    const auto po = b.CreateVector(pathOffsets);
    const auto pc = b.CreateVector(pathComponents);
    const auto pp = b.CreateVector(pathPrefixes);
    const auto pv = b.CreateVector(placementVolumes);
    const auto pd = b.CreateVector(placementDaughters);
    const auto pt = b.CreateVector(placementSets), dp = b.CreateVector(degradationPaths);
    const auto df = b.CreateVector(degradationFlags);
    const auto source = b.CreateSharedString(m.defaultSourceSystem);
    const auto metadata = fbs::CreateMetadataColumns(b, tagSets, po, pc, pp, vt, pv, pd, pt, ot, dn,
                                                     pn, op, dp, df, source, ss);
    return {vol, metadata, dictionary.encode(b)};
}
template <class... Vectors> void equalColumns(std::size_t count, const Vectors *...columns) {
    if (((columns->size() != count) || ...))
        throw std::runtime_error("NHS9: mismatched column lengths");
}
template <class Offsets>
void checkOffsets(const Offsets *offsets, std::size_t rows, std::size_t elements) {
    if (offsets->size() != rows + 1 || offsets->Get(0) != 0 ||
        offsets->Get(columnIndex(rows)) != elements)
        throw std::runtime_error("NHS9: invalid column offsets");
    for (uint32_t i = 1; i < offsets->size(); ++i)
        if (offsets->Get(i) < offsets->Get(i - 1))
            throw std::runtime_error("NHS9: invalid column offsets");
}
template <class Catalog> uint64_t referencedId(const Catalog *catalog, uint32_t index) {
    if (!index || index > catalog->size())
        throw std::runtime_error("NHS9: invalid catalog reference");
    return catalog->Get(index - 1)->id();
}
void decodeColumns(const fbs::SharedGeometry *fb, Scene &scene) {
    const auto dictionary = decodeStrings(fb->strings());
    const auto *v = fb->volumes();
    const DeltaColumn<uint64_t> ids{v->id_deltas()};
    const IndexColumn daughterVolumes{v->daughter_volume_deltas()};
    const StringColumn names{v->name_index_deltas(), dictionary, true};
    const StringColumn daughterNames{v->daughter_name_indices(), dictionary, false};
    const IndexColumn offsets{v->daughter_offset_deltas()};
    const auto n = ids.size();
    const auto d = daughterNames.size();
    equalColumns(n, &names, v->shapes(), v->materials());
    equalColumns(d, &daughterVolumes, v->daughter_transforms());
    checkOffsets(&offsets, n, d);
    // Validated columns give the exact catalog size; avoid repeated reallocations.
    scene.logVols.reserve(n);
    auto volumeId = [&](uint32_t index) {
        if (index == 0 || index > n)
            throw std::runtime_error("NHS9: invalid volume reference");
        return LogVolId{ids.Get(index - 1)};
    };
    for (uint32_t i = 0; i < n; ++i) {
        LogicalVolume value{LogVolId{ids.Get(i)}, names.Get(i),
                            ShapeId{referencedId(fb->shapes(), v->shapes()->Get(i))},
                            MaterialId{referencedId(fb->materials(), v->materials()->Get(i))}};
        const auto begin = offsets.Get(i), end = offsets.Get(i + 1);
        value.daughters.reserve(end - begin);
        for (auto j = begin; j < end; ++j)
            value.daughters.push_back(
                {daughterNames.Get(j), volumeId(daughterVolumes.Get(j)),
                 decodeTransform(fb->transforms(), v->daughter_transforms()->Get(j))});
        insertUnique(scene.logVols, value.id, std::move(value));
    }
    const auto *meta = fb->metadata();
    auto &m = scene.metadata;
    const IndexColumn pathOffsets{meta->path_offset_deltas()};
    if (pathOffsets.size() == 0)
        throw std::runtime_error("NHS9: invalid column offsets");
    const auto pathCount = pathOffsets.size() - 1;
    checkOffsets(&pathOffsets, pathCount, meta->path_components()->size());
    equalColumns(pathCount, meta->path_prefix_lengths());
    std::vector<OccurrenceId> paths;
    paths.reserve(pathCount);
    std::size_t totalComponents = 0;
    for (uint32_t i = 0; i < pathCount; ++i) {
        const auto prefix = meta->path_prefix_lengths()->Get(i);
        const auto begin = pathOffsets.Get(i), end = pathOffsets.Get(i + 1);
        if (prefix > (paths.empty() ? 0 : paths.back().size()))
            throw std::runtime_error("NHS9: invalid path prefix");
        addDecodedSize(totalComponents, prefix, end - begin,
                       FLATBUFFERS_MAX_BUFFER_SIZE / sizeof(uint64_t) - 1,
                       "NHS9: decoded paths exceed size limit");
        OccurrenceId value;
        value.reserve(static_cast<std::size_t>(prefix) + end - begin);
        if (!paths.empty())
            value.assign(paths.back().begin(),
                         paths.back().begin() + static_cast<OccurrenceId::difference_type>(prefix));
        for (auto j = begin; j < end; ++j)
            value.push_back(indexOf(meta->path_components()->Get(j)));
        paths.push_back(std::move(value));
    }
    auto path = [&](uint32_t index) {
        if (index >= paths.size())
            throw std::runtime_error("NHS9: invalid path reference");
        return paths[index];
    };
    for (const auto *set : *meta->tag_sets()) {
        OccurrenceTags tags;
        for (const auto *tag : *set->tags())
            insertUnique(tags, tag->key()->str(), tag->value()->str());
        m.tagSets.push_back(std::move(tags));
    }
    auto pairs = [&](const fbs::IndexPairs *rows, auto &target, auto key) {
        const IndexColumn keys{rows->key_deltas()};
        equalColumns(keys.size(), rows->values());
        for (uint32_t i = 0; i < keys.size(); ++i)
            insertUnique(target, key(keys.Get(i)), rows->values()->Get(i));
    };
    pairs(meta->volume_tags(), m.volumeTags, volumeId);
    pairs(meta->occurrence_tags(), m.occurrenceTags, path);
    equalColumns(meta->placement_volumes()->size(), meta->placement_daughters(),
                 meta->placement_tag_sets());
    for (uint32_t i = 0; i < meta->placement_volumes()->size(); ++i)
        insertUnique(m.placementTags,
                     PlacementKey{volumeId(meta->placement_volumes()->Get(i)),
                                  indexOf(meta->placement_daughters()->Get(i))},
                     meta->placement_tag_sets()->Get(i));
    auto strings = [&](const fbs::StringOverrides *rows, auto &target) {
        const IndexColumn pathIndices{rows->path_deltas()};
        const StringColumn values{rows->value_index_deltas(), dictionary, true};
        equalColumns(pathIndices.size(), &values);
        for (uint32_t i = 0; i < pathIndices.size(); ++i)
            insertUnique(target, path(pathIndices.Get(i)), values.Get(i));
    };
    strings(meta->display_names(), m.displayNames);
    strings(meta->path_names(), m.pathNames);
    strings(meta->original_paths(), m.originalPaths);
    strings(meta->source_systems(), m.sourceSystems);
    m.defaultSourceSystem = meta->default_source_system()->str();
    equalColumns(meta->degradation_paths()->size(), meta->degradation_flags());
    for (uint32_t i = 0; i < meta->degradation_paths()->size(); ++i) {
        const auto bits = meta->degradation_flags()->Get(i);
        if (bits >> static_cast<unsigned>(DegradationBit::Count_))
            throw std::runtime_error("NHS9: unsupported degradation flags");
        DegradationFlags flags;
        flags.bits = decltype(flags.bits){bits};
        insertUnique(m.degradation, path(meta->degradation_paths()->Get(i)), flags);
    }
}
} // namespace

std::vector<std::byte> sceneToBytes(const Scene &geometry) {
    const auto count = geometry.validate();
    flatbuffers::FlatBufferBuilder b;
    // Preserve scalar signed zero as well as ordinary numerical values.
    b.ForceDefaults(true);
    TransformPoolBuild transforms;
    auto placement = [&](const semantic::DaughterPlacement &p) {
        auto name = b.CreateSharedString(p.name);
        auto transform = transforms.internTransform(p.localTransform);
        return fbs::CreatePlacement(b, name, p.logVolId.value, transform);
    };
    const auto &scene = geometry;
    std::vector<flatbuffers::Offset<fbs::Material>> materials;
    for (auto id : sortedKeys(scene.materials)) {
        const auto &m = scene.materials.at(id);
        auto name = b.CreateSharedString(m.name);
        fbs::Vec3f color;
        if (m.color)
            color = fbs::Vec3f{m.color->x, m.color->y, m.color->z};
        materials.push_back(fbs::CreateMaterial(b, id.value, name, m.color.has_value(),
                                                m.color ? &color : nullptr, m.density));
    }
    std::vector<flatbuffers::Offset<fbs::Shape>> shapes;
    for (auto id : sortedKeys(scene.shapes)) {
        auto s = serializeShapeVariant(b, transforms, scene.shapes.at(id).data);
        shapes.push_back(fbs::CreateShape(b, id.value, s.type, s.offset));
    }
    const auto columns = encodeColumns(b, transforms, scene);
    auto root = placement(geometry.root);
    auto sourceFile = b.CreateString(scene.sourceFile);
    auto tf = b.CreateVector(transforms.values);
    auto mats = b.CreateVector(materials);
    auto sh = b.CreateVector(shapes);
    uint64_t features = 0;
    for (const auto &[id, shape] : scene.shapes) {
        if (std::holds_alternative<ExtrudedShape>(shape.data) ||
            std::holds_alternative<GenericTrapShape>(shape.data))
            features |= 1;
    }
    auto result = fbs::CreateSharedGeometry(b, 6, features, sourceFile, root, count, tf, mats, sh,
                                            columns.volumes, columns.metadata, columns.strings);
    fbs::FinishSharedGeometryBuffer(b, result);
    const auto *begin = reinterpret_cast<const std::byte *>(b.GetBufferPointer());
    return {begin, begin + b.GetSize()};
}

Scene sceneFromBytes(std::span<const std::byte> bytes) {
    std::vector<std::byte> decompressed;
    if (detail::zstd_io::isCompressed(bytes)) {
        // Refuse oversized frames before allocating. NHS9 remains one FlatBuffer.
        const auto size = ZSTD_getFrameContentSize(bytes.data(), bytes.size());
        if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR ||
            size >= FLATBUFFERS_MAX_BUFFER_SIZE)
            throw std::runtime_error("NHB: invalid or oversized zstd frame");
        const auto compressedSize = ZSTD_findFrameCompressedSize(bytes.data(), bytes.size());
        if (ZSTD_isError(compressedSize) || compressedSize != bytes.size())
            throw std::runtime_error("NHB: truncated or trailing compressed data");
        decompressed = detail::zstd_io::decompress(bytes);
        bytes = decompressed;
    }
    if (bytes.size() < 8 || bytes.size() >= FLATBUFFERS_MAX_BUFFER_SIZE)
        throw std::runtime_error("NHB: invalid FlatBuffer size");
    if (std::memcmp(bytes.data() + 4, "NHS8", 4) == 0) {
        auto legacy = legacy::nhs8::semanticSceneFromBytes(bytes);
        legacy.computeWorldTransforms();
        legacy.computeOriginalPaths();
        return fromExpanded(legacy);
    }
    if (std::memcmp(bytes.data() + 4, "NHS9", 4) != 0)
        throw std::runtime_error("NHB: unsupported format identifier (expected NHS8 or NHS9)");
    flatbuffers::Verifier::Options options;
    // Millions of source definitions are valid; verification work remains bounded
    // by the buffer length rather than the much larger physical occurrence count.
    options.max_tables = static_cast<flatbuffers::uoffset_t>(bytes.size() / 4);
    flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(),
                                   options);
    if (!fbs::VerifySharedGeometryBuffer(verifier))
        throw std::runtime_error("NHS9: invalid FlatBuffer");
    const auto *fb = fbs::GetSharedGeometry(bytes.data());
    if (fb->schema_version() != 6 || (fb->required_features() & ~uint64_t{1}) != 0)
        throw std::runtime_error("NHS9: unsupported schema version or required features");
    if (fb->transforms()->size() % 16)
        throw std::runtime_error("NHS9: malformed transform pool");
    Scene result;
    auto &scene = result;
    scene.sourceFile = fb->source_file()->str();
    auto placement = [&](const fbs::Placement *p) {
        return semantic::DaughterPlacement{p->name()->str(), semantic::LogVolId{p->log_vol_id()},
                                           decodeTransform(fb->transforms(), p->transform_index())};
    };
    result.root = placement(fb->root());
    scene.materials.reserve(fb->materials()->size());
    scene.shapes.reserve(fb->shapes()->size());
    for (const auto *m : *fb->materials()) {
        semantic::SourceMaterial value{semantic::MaterialId{m->id()},
                                       m->name() ? m->name()->str() : "", std::nullopt,
                                       m->density()};
        if (m->has_color()) {
            if (!m->color())
                throw std::runtime_error("NHS9: missing material color");
            value.color = glm::vec3{m->color()->x(), m->color()->y(), m->color()->z()};
        }
        insertUnique(scene.materials, value.id, value);
    }
    for (const auto *s : *fb->shapes()) {
        semantic::Shape value{semantic::ShapeId{s->id()},
                              deserializeShapeVariant(s->data_type(), s->data(), fb->transforms())};
        insertUnique(scene.shapes, value.id, std::move(value));
    }
    decodeColumns(fb, result);
    if (result.validate() != fb->occurrence_count())
        throw std::runtime_error("NHS9: occurrence count mismatch");
    result.reseedIdCounters();
    return result;
}
void writeFlatbuffer(const Scene &geometry, const std::filesystem::path &path,
                     int compressionLevel) {
    const auto bytes = sceneToBytes(geometry);
    detail::zstd_io::writeBytesToFile(path, bytes, compressionLevel);
}
Scene readFlatbuffer(const std::filesystem::path &path) {
    return sceneFromBytes(detail::file_io::readFile(path));
}
} // namespace nodehammer::ir::semantic

namespace nodehammer::ir {
std::vector<std::byte> semanticSceneToBytes(const semantic::Scene &scene) {
    return semantic::sceneToBytes(scene);
}
semantic::Scene semanticSceneFromBytes(std::span<const std::byte> bytes) {
    return semantic::sceneFromBytes(bytes);
}
} // namespace nodehammer::ir
