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
OccurrenceId pathOf(const flatbuffers::Vector<uint64_t> *values) {
    OccurrenceId result;
    result.reserve(values->size());
    for (auto v : *values)
        result.push_back(indexOf(v));
    return result;
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
auto encodePath(flatbuffers::FlatBufferBuilder &b, const OccurrenceId &path) {
    return b.CreateVector(std::vector<uint64_t>{path.begin(), path.end()});
}
auto encodeStrings(flatbuffers::FlatBufferBuilder &b,
                   const std::map<OccurrenceId, std::string> &source) {
    std::vector<flatbuffers::Offset<fbs::OccurrenceString>> rows;
    for (const auto &[path, value] : source) {
        auto p = encodePath(b, path);
        auto v = b.CreateSharedString(value);
        rows.push_back(fbs::CreateOccurrenceString(b, p, v));
    }
    return b.CreateVector(rows);
}
void decodeStrings(const flatbuffers::Vector<flatbuffers::Offset<fbs::OccurrenceString>> *rows,
                   std::map<OccurrenceId, std::string> &destination) {
    for (const auto *row : *rows)
        insertUnique(destination, pathOf(row->path()), row->value()->str());
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
    std::vector<flatbuffers::Offset<fbs::Volume>> volumes;
    for (auto id : sortedKeys(scene.logVols)) {
        const auto &v = scene.logVols.at(id);
        std::vector<flatbuffers::Offset<fbs::Placement>> daughters;
        daughters.reserve(v.daughters.size());
        for (const auto &d : v.daughters)
            daughters.push_back(placement(d));
        auto name = b.CreateSharedString(v.name);
        auto ds = b.CreateVector(daughters);
        volumes.push_back(
            fbs::CreateVolume(b, id.value, name, v.shapeId.value, v.materialId.value, ds));
    }
    const auto &m = geometry.metadata;
    std::vector<flatbuffers::Offset<fbs::TagSet>> tagSets;
    for (const auto &tags : m.tagSets) {
        std::vector<flatbuffers::Offset<fbs::Tag>> entries;
        for (const auto &[key, value] : tags) {
            auto k = b.CreateSharedString(key), v = b.CreateSharedString(value);
            entries.push_back(fbs::CreateTag(b, k, v));
        }
        tagSets.push_back(fbs::CreateTagSet(b, b.CreateVector(entries)));
    }
    std::vector<flatbuffers::Offset<fbs::VolumeTags>> volumeTags;
    for (const auto &[id, tags] : m.volumeTags)
        volumeTags.push_back(fbs::CreateVolumeTags(b, id.value, tags));
    std::vector<flatbuffers::Offset<fbs::PlacementTags>> placementTags;
    for (const auto &[key, tags] : m.placementTags)
        placementTags.push_back(fbs::CreatePlacementTags(b, key.first.value, key.second, tags));
    std::vector<flatbuffers::Offset<fbs::OccurrenceTags>> occurrenceTags;
    for (const auto &[path, tags] : m.occurrenceTags)
        occurrenceTags.push_back(fbs::CreateOccurrenceTags(b, encodePath(b, path), tags));
    std::vector<flatbuffers::Offset<fbs::OccurrenceDegradation>> degradation;
    for (const auto &[path, flags] : m.degradation)
        degradation.push_back(
            fbs::CreateOccurrenceDegradation(b, encodePath(b, path), flags.bits.to_ullong()));
    // Build all children before the table: FlatBuffer tables cannot be nested.
    auto ts = b.CreateVector(tagSets);
    auto vt = b.CreateVector(volumeTags);
    auto pt = b.CreateVector(placementTags);
    auto ot = b.CreateVector(occurrenceTags);
    auto dn = encodeStrings(b, m.displayNames), pn = encodeStrings(b, m.pathNames);
    auto op = encodeStrings(b, m.originalPaths), ss = encodeStrings(b, m.sourceSystems);
    auto deg = b.CreateVector(degradation);
    auto sourceSystem = b.CreateSharedString(m.defaultSourceSystem);
    auto metadata = fbs::CreateMetadata(b, ts, vt, pt, ot, dn, pn, op, deg, sourceSystem, ss);
    auto root = placement(geometry.root);
    auto sourceFile = b.CreateString(scene.sourceFile);
    auto tf = b.CreateVector(transforms.values);
    auto mats = b.CreateVector(materials);
    auto sh = b.CreateVector(shapes);
    auto vs = b.CreateVector(volumes);
    auto result =
        fbs::CreateSharedGeometry(b, 1, 0, sourceFile, root, count, tf, mats, sh, vs, metadata);
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
    if (fb->schema_version() != 1 || fb->required_features() != 0)
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
    for (const auto *v : *fb->volumes()) {
        semantic::LogicalVolume value{semantic::LogVolId{v->id()}, v->name()->str(),
                                      semantic::ShapeId{v->shape_id()},
                                      semantic::MaterialId{v->material_id()}};
        value.daughters.reserve(v->daughters()->size());
        for (const auto *d : *v->daughters())
            value.daughters.push_back(placement(d));
        insertUnique(scene.logVols, value.id, std::move(value));
    }
    auto &m = result.metadata;
    const auto *meta = fb->metadata();
    for (const auto *set : *meta->tag_sets()) {
        OccurrenceTags tags;
        for (const auto *tag : *set->tags())
            insertUnique(tags, tag->key()->str(), tag->value()->str());
        m.tagSets.push_back(std::move(tags));
    }
    for (const auto *row : *meta->volume_tags())
        insertUnique(m.volumeTags, semantic::LogVolId{row->volume()}, row->tag_set());
    for (const auto *row : *meta->placement_tags())
        insertUnique(m.placementTags,
                     PlacementKey{semantic::LogVolId{row->volume()}, indexOf(row->daughter())},
                     row->tag_set());
    for (const auto *row : *meta->occurrence_tags())
        insertUnique(m.occurrenceTags, pathOf(row->path()), row->tag_set());
    decodeStrings(meta->display_names(), m.displayNames);
    decodeStrings(meta->path_names(), m.pathNames);
    decodeStrings(meta->original_paths(), m.originalPaths);
    decodeStrings(meta->source_systems(), m.sourceSystems);
    m.defaultSourceSystem = meta->default_source_system()->str();
    for (const auto *row : *meta->degradation()) {
        if (row->flags() >> static_cast<unsigned>(DegradationBit::Count_))
            throw std::runtime_error("NHS9: unsupported degradation flags");
        DegradationFlags flags;
        flags.bits = decltype(flags.bits){row->flags()};
        insertUnique(m.degradation, pathOf(row->path()), flags);
    }
    if (result.validate() != fb->occurrence_count())
        throw std::runtime_error("NHS9: occurrence count mismatch");
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
