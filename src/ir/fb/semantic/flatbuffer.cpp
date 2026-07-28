#include <nodehammer/ir/fb/semantic/flatbuffer.hpp>

#include <nodehammer/detail/overloaded.hpp>

#include <flatbuffers/flatbuffers.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace nodehammer {

namespace {

uint32_t toU32Checked(uint64_t value, std::string_view what) {
    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::format("{} {} exceeds uint32 range", what, value));
    }
    return static_cast<uint32_t>(value);
}

uint8_t toU8Checked(std::size_t value, std::string_view what) {
    if (value > std::numeric_limits<uint8_t>::max()) {
        throw std::runtime_error(std::format("{} {} exceeds uint8 range", what, value));
    }
    return static_cast<uint8_t>(value);
}

using RotationKey = std::array<uint64_t, 9>;
using TranslationKey = std::array<uint64_t, 3>;
using TransformKey = std::array<uint32_t, 2>;

RotationKey rotationKey(const glm::dmat4 &m) {
    RotationKey key{};
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            key[static_cast<std::size_t>(col * 3 + row)] = std::bit_cast<uint64_t>(m[col][row]);
        }
    }
    return key;
}

TranslationKey translationKey(const glm::dmat4 &m) {
    return {
        std::bit_cast<uint64_t>(m[3][0]),
        std::bit_cast<uint64_t>(m[3][1]),
        std::bit_cast<uint64_t>(m[3][2]),
    };
}

struct TransformPoolBuild {
    std::vector<double> rotations;    // 9 doubles per row
    std::vector<double> translations; // 3 doubles per row
    std::vector<uint32_t> transformRotIndices;
    std::vector<uint32_t> transformTrlIndices;

    std::map<RotationKey, uint32_t> rotDedup;
    std::map<TranslationKey, uint32_t> trlDedup;
    std::map<TransformKey, uint32_t> tfDedup;

    TransformPoolBuild() {
        // row 0: identity / zero
        rotations = {
            1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0,
        };
        translations = {0.0, 0.0, 0.0};

        RotationKey r0{};
        r0[0] = std::bit_cast<uint64_t>(1.0);
        r0[4] = std::bit_cast<uint64_t>(1.0);
        r0[8] = std::bit_cast<uint64_t>(1.0);
        TranslationKey t0{};

        rotDedup.emplace(r0, 0);
        trlDedup.emplace(t0, 0);

        transformRotIndices.push_back(0);
        transformTrlIndices.push_back(0);
        tfDedup.emplace(TransformKey{0, 0}, 0);
    }

    uint32_t internRotation(const glm::dmat4 &m) {
        const auto key = rotationKey(m);
        auto [it, inserted] =
            rotDedup.try_emplace(key, static_cast<uint32_t>(rotations.size() / 9));
        if (inserted) {
            for (int col = 0; col < 3; ++col) {
                for (int row = 0; row < 3; ++row) {
                    rotations.push_back(m[col][row]);
                }
            }
        }
        return it->second;
    }

    uint32_t internTranslation(const glm::dmat4 &m) {
        const auto key = translationKey(m);
        auto [it, inserted] =
            trlDedup.try_emplace(key, static_cast<uint32_t>(translations.size() / 3));
        if (inserted) {
            translations.push_back(m[3][0]);
            translations.push_back(m[3][1]);
            translations.push_back(m[3][2]);
        }
        return it->second;
    }

    uint32_t internTransform(const glm::dmat4 &m) {
        const uint32_t r = internRotation(m);
        const uint32_t t = internTranslation(m);
        auto [it, inserted] = tfDedup.try_emplace(
            TransformKey{r, t}, static_cast<uint32_t>(transformRotIndices.size()));
        if (inserted) {
            transformRotIndices.push_back(r);
            transformTrlIndices.push_back(t);
        }
        return it->second;
    }
};

glm::dmat4 decodeTransform(const flatbuffers::Vector<double> *rots,
                           const flatbuffers::Vector<double> *trls,
                           const flatbuffers::Vector<uint32_t> *tfRotIdx,
                           const flatbuffers::Vector<uint32_t> *tfTrlIdx, uint32_t tfIndex) {
    glm::dmat4 m{1.0};
    if (!rots || !trls || !tfRotIdx || !tfTrlIdx || tfIndex >= tfRotIdx->size() ||
        tfIndex >= tfTrlIdx->size()) {
        return m;
    }

    const auto rIdx = tfRotIdx->Get(tfIndex);
    const auto tIdx = tfTrlIdx->Get(tfIndex);
    const auto rBase = static_cast<flatbuffers::uoffset_t>(rIdx * 9);
    const auto tBase = static_cast<flatbuffers::uoffset_t>(tIdx * 3);

    if (rBase + 8 < rots->size()) {
        for (flatbuffers::uoffset_t col = 0; col < 3; ++col) {
            for (flatbuffers::uoffset_t row = 0; row < 3; ++row) {
                m[static_cast<int>(col)][static_cast<int>(row)] = rots->Get(rBase + col * 3 + row);
            }
        }
    }
    if (tBase + 2 < trls->size()) {
        m[3][0] = trls->Get(tBase);
        m[3][1] = trls->Get(tBase + 1);
        m[3][2] = trls->Get(tBase + 2);
    }

    return m;
}

std::string formatBytes(std::size_t bytes) {
    constexpr double k = 1024.0;
    const double kib = static_cast<double>(bytes) / k;
    const double mib = kib / k;
    if (mib >= 1.0) {
        return std::format("{:.2f} MiB", mib);
    }
    if (kib >= 1.0) {
        return std::format("{:.2f} KiB", kib);
    }
    return std::format("{} B", bytes);
}

// ── Mat4x4 helpers ──────────────────────────────────────────────────────────

// ── Shape serialization ─────────────────────────────────────────────────────

struct ShapeOffsetResult {
    fbs::ShapeData type;
    flatbuffers::Offset<void> offset;
};

ShapeOffsetResult serializeShapeVariant(flatbuffers::FlatBufferBuilder &builder,
                                        TransformPoolBuild &transformPool,
                                        const SemanticShapeVariant &data) {
    return std::visit(
        detail::overloaded{
            [&](const BoxShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateBoxShape(builder, s.dx, s.dy, s.dz);
                return {fbs::ShapeData_BoxShape, o.Union()};
            },
            [&](const TubeShape &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateTubeShape(builder, s.rMin, s.rMax, s.dz, s.phiStart, s.phiDelta);
                return {fbs::ShapeData_TubeShape, o.Union()};
            },
            [&](const ConeShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateConeShape(builder, s.rMin1, s.rMax1, s.rMin2, s.rMax2, s.dz,
                                              s.phiStart, s.phiDelta);
                return {fbs::ShapeData_ConeShape, o.Union()};
            },
            [&](const TrdShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateTrdShape(builder, s.dx1, s.dx2, s.dy1, s.dy2, s.dz);
                return {fbs::ShapeData_TrdShape, o.Union()};
            },
            [&](const ParaShape &s) -> ShapeOffsetResult {
                auto o = fbs::CreateParaShape(builder, s.dx, s.dy, s.dz, s.alpha, s.theta, s.phi);
                return {fbs::ShapeData_ParaShape, o.Union()};
            },
            [&](const PconShape &s) -> ShapeOffsetResult {
                std::vector<fbs::Section> secs;
                secs.reserve(s.sections.size());
                for (const auto &sec : s.sections) {
                    secs.emplace_back(sec.z, sec.rMin, sec.rMax);
                }
                auto o = fbs::CreatePconShape(builder, s.phiStart, s.phiDelta,
                                              builder.CreateVectorOfStructs(secs));
                return {fbs::ShapeData_PconShape, o.Union()};
            },
            [&](const PgonShape &s) -> ShapeOffsetResult {
                std::vector<fbs::Section> secs;
                secs.reserve(s.sections.size());
                for (const auto &sec : s.sections) {
                    secs.emplace_back(sec.z, sec.rMin, sec.rMax);
                }
                auto o = fbs::CreatePgonShape(builder, s.phiStart, s.phiDelta, s.nSides,
                                              builder.CreateVectorOfStructs(secs));
                return {fbs::ShapeData_PgonShape, o.Union()};
            },
            [&](const TorusShape &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateTorusShape(builder, s.rMin, s.rMax, s.rTor, s.phiStart, s.phiDelta);
                return {fbs::ShapeData_TorusShape, o.Union()};
            },
            [&](const TessellatedShape &s) -> ShapeOffsetResult {
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
            [&](const BooleanUnion &s) -> ShapeOffsetResult {
                auto o = fbs::CreateBooleanUnion(builder, toU32Checked(s.left.value, "shape id"),
                                                 toU32Checked(s.right.value, "shape id"),
                                                 transformPool.internTransform(s.rightTransform));
                return {fbs::ShapeData_BooleanUnion, o.Union()};
            },
            [&](const BooleanIntersection &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateBooleanIntersection(builder, toU32Checked(s.left.value, "shape id"),
                                                   toU32Checked(s.right.value, "shape id"),
                                                   transformPool.internTransform(s.rightTransform));
                return {fbs::ShapeData_BooleanIntersection, o.Union()};
            },
            [&](const BooleanSubtraction &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateBooleanSubtraction(builder, toU32Checked(s.left.value, "shape id"),
                                                  toU32Checked(s.right.value, "shape id"),
                                                  transformPool.internTransform(s.rightTransform));
                return {fbs::ShapeData_BooleanSubtraction, o.Union()};
            },
            [&](const UnknownShape &s) -> ShapeOffsetResult {
                auto o =
                    fbs::CreateUnknownShape(builder, builder.CreateSharedString(s.originalType));
                return {fbs::ShapeData_UnknownShape, o.Union()};
            },
        },
        data);
}

// ── Shape deserialization ───────────────────────────────────────────────────

SemanticShapeVariant deserializeShapeVariant(fbs::ShapeData type, const void *data,
                                             const flatbuffers::Vector<double> *poolRots,
                                             const flatbuffers::Vector<double> *poolTrls,
                                             const flatbuffers::Vector<uint32_t> *poolTfRotIdx,
                                             const flatbuffers::Vector<uint32_t> *poolTfTrlIdx) {
    switch (type) {
    case fbs::ShapeData_BoxShape: {
        const auto *s = static_cast<const fbs::BoxShape *>(data);
        return BoxShape{s->dx(), s->dy(), s->dz()};
    }
    case fbs::ShapeData_TubeShape: {
        const auto *s = static_cast<const fbs::TubeShape *>(data);
        return TubeShape{s->r_min(), s->r_max(), s->dz(), s->phi_start(), s->phi_delta()};
    }
    case fbs::ShapeData_ConeShape: {
        const auto *s = static_cast<const fbs::ConeShape *>(data);
        return ConeShape{s->r_min1(), s->r_max1(),    s->r_min2(),   s->r_max2(),
                         s->dz(),     s->phi_start(), s->phi_delta()};
    }
    case fbs::ShapeData_TrdShape: {
        const auto *s = static_cast<const fbs::TrdShape *>(data);
        return TrdShape{s->dx1(), s->dx2(), s->dy1(), s->dy2(), s->dz()};
    }
    case fbs::ShapeData_ParaShape: {
        const auto *s = static_cast<const fbs::ParaShape *>(data);
        return ParaShape{s->dx(), s->dy(), s->dz(), s->alpha(), s->theta(), s->phi()};
    }
    case fbs::ShapeData_PconShape: {
        const auto *s = static_cast<const fbs::PconShape *>(data);
        PconShape result;
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
        PgonShape result;
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
        return TorusShape{s->r_min(), s->r_max(), s->r_tor(), s->phi_start(), s->phi_delta()};
    }
    case fbs::ShapeData_TessellatedShape: {
        const auto *s = static_cast<const fbs::TessellatedShape *>(data);
        TessellatedShape result;
        if (s->triangles()) {
            result.triangles.reserve(s->triangles()->size());
            for (const auto *tri : *s->triangles()) {
                TessellatedShape::Triangle t;
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
        BooleanUnion result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = decodeTransform(poolRots, poolTrls, poolTfRotIdx, poolTfTrlIdx,
                                                s->right_transform_index());
        return result;
    }
    case fbs::ShapeData_BooleanIntersection: {
        const auto *s = static_cast<const fbs::BooleanIntersection *>(data);
        BooleanIntersection result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = decodeTransform(poolRots, poolTrls, poolTfRotIdx, poolTfTrlIdx,
                                                s->right_transform_index());
        return result;
    }
    case fbs::ShapeData_BooleanSubtraction: {
        const auto *s = static_cast<const fbs::BooleanSubtraction *>(data);
        BooleanSubtraction result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = decodeTransform(poolRots, poolTrls, poolTfRotIdx, poolTfTrlIdx,
                                                s->right_transform_index());
        return result;
    }
    case fbs::ShapeData_UnknownShape: {
        const auto *s = static_cast<const fbs::UnknownShape *>(data);
        return UnknownShape{s->original_type() ? s->original_type()->str() : ""};
    }
    default:
        return UnknownShape{"flatbuffer_unknown"};
    }
}

} // namespace

// ── Layer 1: Type conversion ────────────────────────────────────────────────

flatbuffers::Offset<fbs::SemanticScene>
semanticSceneToFlatBuffer(flatbuffers::FlatBufferBuilder &builder,
                          const detail::SemanticScene &scene) {
    TransformPoolBuild transformPool;

    // Collect nodes into a stable order (sorted by ID for reproducibility).
    std::vector<const SemanticNode *> orderedNodes;
    orderedNodes.reserve(scene.nodes.size());
    for (const auto &[id, node] : scene.nodes) {
        (void)id;
        orderedNodes.push_back(&node);
    }
    std::sort(orderedNodes.begin(), orderedNodes.end(),
              [](const SemanticNode *a, const SemanticNode *b) { return a->id < b->id; });
    const auto N = orderedNodes.size();

    // Remap logical volume IDs to a dense local range [1..M].
    std::vector<SemanticLogVolId> orderedLogVolIds;
    orderedLogVolIds.reserve(scene.logVols.size());
    for (const auto &[id, lv] : scene.logVols) {
        (void)lv;
        orderedLogVolIds.push_back(id);
    }
    std::sort(orderedLogVolIds.begin(), orderedLogVolIds.end());

    std::unordered_map<SemanticLogVolId, uint32_t> logVolIdRemap;
    logVolIdRemap.reserve(orderedLogVolIds.size());
    for (std::size_t i = 0; i < orderedLogVolIds.size(); ++i) {
        logVolIdRemap.emplace(orderedLogVolIds[i], toU32Checked(i + 1, "logvol remap index"));
    }

    auto remapLogVolId = [&](SemanticLogVolId id, std::string_view what) -> uint32_t {
        if (const auto it = logVolIdRemap.find(id); it != logVolIdRemap.end()) {
            return it->second;
        }
        throw std::runtime_error(std::format("{} {} missing from logvol remap", what, id.value));
    };

    // Pre-intern all transforms globally so indices are stable across sections.
    for (const auto *node : orderedNodes) {
        transformPool.internTransform(node->localTransform);
    }
    for (const auto id : orderedLogVolIds) {
        const auto &lv = scene.logVols.at(id);
        for (const auto &d : lv.daughters) {
            transformPool.internTransform(d.localTransform);
        }
    }
    for (const auto &[id, shape] : scene.shapes) {
        (void)id;
        std::visit(
            detail::overloaded{
                [&](const BooleanUnion &s) { transformPool.internTransform(s.rightTransform); },
                [&](const BooleanIntersection &s) {
                    transformPool.internTransform(s.rightTransform);
                },
                [&](const BooleanSubtraction &s) {
                    transformPool.internTransform(s.rightTransform);
                },
                [&](const auto &) {},
            },
            shape.data);
    }

    // ── Shapes ──────────────────────────────────────────────────────────────
    std::vector<flatbuffers::Offset<fbs::Shape>> shapeOffsets;
    shapeOffsets.reserve(scene.shapes.size());
    for (const auto &[id, shape] : scene.shapes) {
        auto [dataType, dataOffset] = serializeShapeVariant(builder, transformPool, shape.data);
        auto o = fbs::CreateShape(builder, toU32Checked(shape.id.value, "shape id"), dataType,
                                  dataOffset);
        shapeOffsets.push_back(o);
    }

    // ── Materials ───────────────────────────────────────────────────────────
    std::vector<flatbuffers::Offset<fbs::Material>> matOffsets;
    matOffsets.reserve(scene.materials.size());
    for (const auto &[id, mat] : scene.materials) {
        auto nameOff = builder.CreateSharedString(mat.name);
        fbs::Vec3f color{0.0f, 0.0f, 0.0f};
        bool hasColor = mat.color.has_value();
        if (hasColor) {
            color = fbs::Vec3f{mat.color->x, mat.color->y, mat.color->z};
        }
        auto o = fbs::CreateMaterial(builder, toU32Checked(mat.id.value, "material id"), nameOff,
                                     hasColor, &color, mat.density);
        matOffsets.push_back(o);
    }

    // ── Logical Volumes ─────────────────────────────────────────────────────
    std::vector<flatbuffers::Offset<fbs::LogicalVolume>> lvOffsets;
    lvOffsets.reserve(scene.logVols.size());
    for (const auto id : orderedLogVolIds) {
        const auto &lv = scene.logVols.at(id);
        auto nameOff = builder.CreateSharedString(lv.name);

        std::vector<flatbuffers::Offset<fbs::DaughterPlacement>> daughterOffsets;
        daughterOffsets.reserve(lv.daughters.size());
        for (const auto &d : lv.daughters) {
            auto dNameOff = builder.CreateSharedString(d.name);
            const auto dTf = transformPool.internTransform(d.localTransform);
            auto dOff = fbs::CreateDaughterPlacement(
                builder, dNameOff, remapLogVolId(d.logVolId, "daughter logvol"), dTf);
            daughterOffsets.push_back(dOff);
        }

        auto daughtersVec =
            daughterOffsets.empty()
                ? flatbuffers::Offset<
                      flatbuffers::Vector<flatbuffers::Offset<fbs::DaughterPlacement>>>{}
                : builder.CreateVector(daughterOffsets);

        auto o = fbs::CreateLogicalVolume(builder, remapLogVolId(lv.id, "logical volume id"),
                                          nameOff, toU32Checked(lv.shapeId.value, "shape id"),
                                          toU32Checked(lv.materialId.value, "material id"),
                                          daughtersVec);
        lvOffsets.push_back(o);
    }

    // ── Nodes (column-oriented) ────────────────────────────────────────────
    // Parallel arrays
    std::vector<uint32_t> logVolIds32(N);
    std::vector<uint32_t> transformIndices(N);
    std::vector<uint32_t> parentIds(N);
    std::vector<uint8_t> tagCounts(N);
    std::vector<fbs::TagRef> tagRefs;
    std::vector<uint8_t> degradation(N);
    struct NodeRowData {
        const SemanticNode *node{nullptr};
        uint32_t depth{0};
        uint64_t parentOldId{0};
        uint32_t logVolIndex{0};
        uint32_t nameIndex{0};
        uint32_t transformIndex{0};
        uint8_t sourceSystemIndex{0};
        uint8_t degradation{0};
        uint8_t tagCount{0};
    };
    std::vector<NodeRowData> nodeRows;
    nodeRows.reserve(N);

    std::size_t totalTagCount = 0;
    for (const auto *node : orderedNodes) {
        totalTagCount += node->tags.size();
    }
    tagRefs.reserve(totalTagCount);

    // String tables
    std::unordered_map<std::string, uint32_t> nameMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> nameTableOffsets;
    std::vector<uint32_t> nameIndices(N);
    std::unordered_map<std::string, std::size_t> nameFreq;
    nameFreq.reserve(orderedNodes.size());
    for (const auto *node : orderedNodes) {
        ++nameFreq[node->name];
    }
    std::vector<std::pair<std::string, std::size_t>> rankedNames{nameFreq.begin(), nameFreq.end()};
    std::sort(rankedNames.begin(), rankedNames.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });
    for (const auto &[name, _freq] : rankedNames) {
        const auto idx = toU32Checked(nameTableOffsets.size(), "name table index");
        nameMap.emplace(name, idx);
        nameTableOffsets.push_back(builder.CreateSharedString(name));
    }

    std::unordered_map<std::string, uint8_t> tagKeyMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> tagKeyTableOffsets;
    std::unordered_map<std::string, uint8_t> tagValueMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> tagValueTableOffsets;

    std::unordered_map<std::string, uint8_t> srcSysMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> srcSysTableOffsets;
    std::vector<uint8_t> srcSysIndices(N);
    std::unordered_map<std::string, std::size_t> srcSysFreq;
    srcSysFreq.reserve(orderedNodes.size());
    for (const auto *node : orderedNodes) {
        ++srcSysFreq[node->sourceSystem];
    }
    std::vector<std::pair<std::string, std::size_t>> rankedSrcSystems{srcSysFreq.begin(),
                                                                      srcSysFreq.end()};
    std::sort(rankedSrcSystems.begin(), rankedSrcSystems.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    });
    for (const auto &[sourceSystem, _freq] : rankedSrcSystems) {
        const auto idx = toU8Checked(srcSysTableOffsets.size(), "source-system table index");
        srcSysMap.emplace(sourceSystem, idx);
        srcSysTableOffsets.push_back(builder.CreateSharedString(sourceSystem));
    }

    uint32_t maxNodeLogVolId = 0;
    uint32_t maxNodeNameIndex = 0;
    uint32_t maxNodeTransformIndex = 0;
    std::unordered_map<SemanticNodeId, const SemanticNode *> nodeById;
    nodeById.reserve(orderedNodes.size());
    for (const auto *node : orderedNodes) {
        nodeById.emplace(node->id, node);
    }
    std::unordered_map<SemanticNodeId, uint32_t> nodeDepth;
    nodeDepth.reserve(orderedNodes.size());
    auto computeDepth = [&](const SemanticNode *start) -> uint32_t {
        if (!start) {
            return 0;
        }
        if (const auto it = nodeDepth.find(start->id); it != nodeDepth.end()) {
            return it->second;
        }

        std::vector<const SemanticNode *> chain;
        chain.reserve(16);
        const SemanticNode *cur = start;
        while (cur) {
            if (const auto it = nodeDepth.find(cur->id); it != nodeDepth.end()) {
                uint32_t d = it->second;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    ++d;
                    nodeDepth.emplace((*rit)->id, d);
                }
                return nodeDepth.at(start->id);
            }
            chain.push_back(cur);
            if (!cur->parentId) {
                uint32_t d = 0;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    nodeDepth.emplace((*rit)->id, d++);
                }
                return nodeDepth.at(start->id);
            }
            const auto parentIt = nodeById.find(*cur->parentId);
            if (parentIt == nodeById.end()) {
                uint32_t d = 0;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    nodeDepth.emplace((*rit)->id, d++);
                }
                return nodeDepth.at(start->id);
            }
            // Guard against cycles in malformed scenes.
            if (std::find(chain.begin(), chain.end(), parentIt->second) != chain.end()) {
                uint32_t d = 0;
                for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
                    nodeDepth.emplace((*rit)->id, d++);
                }
                return nodeDepth.at(start->id);
            }
            cur = parentIt->second;
        }
        return 0;
    };

    auto internTagKey = [&](const std::string &s) -> uint8_t {
        if (auto it = tagKeyMap.find(s); it != tagKeyMap.end()) {
            return it->second;
        }
        const auto idx = toU8Checked(tagKeyTableOffsets.size(), "tag-key table index");
        tagKeyMap.emplace(s, idx);
        tagKeyTableOffsets.push_back(builder.CreateSharedString(s));
        return idx;
    };

    auto internTagValue = [&](const std::string &s) -> uint8_t {
        if (auto it = tagValueMap.find(s); it != tagValueMap.end()) {
            return it->second;
        }
        const auto idx = toU8Checked(tagValueTableOffsets.size(), "tag-value table index");
        tagValueMap.emplace(s, idx);
        tagValueTableOffsets.push_back(builder.CreateSharedString(s));
        return idx;
    };

    for (const auto *node : orderedNodes) {
        const auto nameIt = nameMap.find(node->name);
        if (nameIt == nameMap.end()) {
            throw std::runtime_error(
                std::format("node name '{}' missing from name table", node->name));
        }

        const auto srcIt = srcSysMap.find(node->sourceSystem);
        if (srcIt == srcSysMap.end()) {
            throw std::runtime_error(std::format(
                "source system '{}' missing from source-system table", node->sourceSystem));
        }

        const auto degr = node->degradation.bits.to_ulong();
        if (degr > std::numeric_limits<uint8_t>::max()) {
            throw std::runtime_error(
                std::format("degradation bitmask {} exceeds uint8 range", degr));
        }

        const auto tagCount = toU8Checked(node->tags.size(), "per-node tag count");

        NodeRowData row;
        row.node = node;
        row.depth = computeDepth(node);
        row.parentOldId = node->parentId ? node->parentId->value : 0;
        row.nameIndex = nameIt->second;
        row.logVolIndex = remapLogVolId(node->logVolId, "node logvol");
        row.transformIndex = transformPool.internTransform(node->localTransform);
        row.sourceSystemIndex = srcIt->second;
        row.degradation = static_cast<uint8_t>(degr);
        row.tagCount = tagCount;
        nodeRows.push_back(row);

        maxNodeNameIndex = std::max(maxNodeNameIndex, row.nameIndex);
        maxNodeLogVolId = std::max(maxNodeLogVolId, row.logVolIndex);
        maxNodeTransformIndex = std::max(maxNodeTransformIndex, row.transformIndex);
    }

    // Reorder row serialization for better column locality/compression.
    std::sort(nodeRows.begin(), nodeRows.end(), [](const auto &a, const auto &b) {
        if (a.depth != b.depth) {
            return a.depth < b.depth;
        }
        if (a.parentOldId != b.parentOldId) {
            return a.parentOldId < b.parentOldId;
        }
        if (a.logVolIndex != b.logVolIndex) {
            return a.logVolIndex < b.logVolIndex;
        }
        if (a.transformIndex != b.transformIndex) {
            return a.transformIndex < b.transformIndex;
        }
        if (a.nameIndex != b.nameIndex) {
            return a.nameIndex < b.nameIndex;
        }
        return a.node->id.value < b.node->id.value;
    });

    std::unordered_map<SemanticNodeId, uint32_t> nodeIdRemap;
    nodeIdRemap.reserve(N);
    for (std::size_t i = 0; i < N; ++i) {
        nodeIdRemap.emplace(nodeRows[i].node->id, static_cast<uint32_t>(i + 1));
    }

    for (std::size_t i = 0; i < N; ++i) {
        const auto &row = nodeRows[i];
        const auto &node = *row.node;

        nameIndices[i] = row.nameIndex;
        logVolIds32[i] = row.logVolIndex;
        transformIndices[i] = row.transformIndex;
        srcSysIndices[i] = row.sourceSystemIndex;
        degradation[i] = row.degradation;
        tagCounts[i] = row.tagCount;

        // Parent
        if (node.parentId) {
            if (auto it = nodeIdRemap.find(*node.parentId); it != nodeIdRemap.end()) {
                parentIds[i] = it->second;
            } else {
                throw std::runtime_error(
                    std::format("parent node id {} missing from node remap", node.parentId->value));
            }
        } else {
            parentIds[i] = 0;
        }

        // Tags: per-node count + flat key/value ref pairs
        for (const auto &[k, v] : node.tags) {
            tagRefs.emplace_back(internTagKey(k), internTagValue(v));
        }
    }
    // Build NodeColumns
    const bool useU16LogVolIds = maxNodeLogVolId <= std::numeric_limits<uint16_t>::max();
    fbs::LogVolIdWidth logVolIdWidth =
        useU16LogVolIds ? fbs::LogVolIdWidth_U16 : fbs::LogVolIdWidth_U32;

    flatbuffers::Offset<flatbuffers::Vector<uint16_t>> ncLogVolIdsU16Vec;
    flatbuffers::Offset<flatbuffers::Vector<uint32_t>> ncLogVolIdsU32Vec;
    std::vector<uint16_t> logVolIds16;
    if (useU16LogVolIds) {
        logVolIds16.reserve(logVolIds32.size());
        for (const auto id : logVolIds32) {
            logVolIds16.push_back(static_cast<uint16_t>(id));
        }
        ncLogVolIdsU16Vec = builder.CreateVector(logVolIds16);
    } else {
        ncLogVolIdsU32Vec = builder.CreateVector(logVolIds32);
    }

    const bool useU16NameIndices = maxNodeNameIndex <= std::numeric_limits<uint16_t>::max();
    fbs::IndexWidth nameIndexWidth = useU16NameIndices ? fbs::IndexWidth_U16 : fbs::IndexWidth_U32;
    flatbuffers::Offset<flatbuffers::Vector<uint16_t>> ncNameIndicesU16Vec;
    flatbuffers::Offset<flatbuffers::Vector<uint32_t>> ncNameIndicesU32Vec;
    std::vector<uint16_t> nameIndices16;
    if (useU16NameIndices) {
        nameIndices16.reserve(nameIndices.size());
        for (const auto idx : nameIndices) {
            nameIndices16.push_back(static_cast<uint16_t>(idx));
        }
        ncNameIndicesU16Vec = builder.CreateVector(nameIndices16);
    } else {
        ncNameIndicesU32Vec = builder.CreateVector(nameIndices);
    }

    const bool useU16TransformIndices =
        maxNodeTransformIndex <= std::numeric_limits<uint16_t>::max();
    fbs::IndexWidth transformIndexWidth =
        useU16TransformIndices ? fbs::IndexWidth_U16 : fbs::IndexWidth_U32;
    flatbuffers::Offset<flatbuffers::Vector<uint16_t>> ncTransformIndicesU16Vec;
    flatbuffers::Offset<flatbuffers::Vector<uint32_t>> ncTransformIndicesU32Vec;
    std::vector<uint16_t> transformIndices16;
    if (useU16TransformIndices) {
        transformIndices16.reserve(transformIndices.size());
        for (const auto idx : transformIndices) {
            transformIndices16.push_back(static_cast<uint16_t>(idx));
        }
        ncTransformIndicesU16Vec = builder.CreateVector(transformIndices16);
    } else {
        ncTransformIndicesU32Vec = builder.CreateVector(transformIndices);
    }

    auto ncNameTableVec = builder.CreateVector(nameTableOffsets);
    auto ncParentIdsVec = builder.CreateVector(parentIds);
    auto ncTagCountsVec = builder.CreateVector(tagCounts);
    auto ncTagKeyTableVec = builder.CreateVector(tagKeyTableOffsets);
    auto ncTagRefsVec = builder.CreateVectorOfStructs(tagRefs);
    auto ncTagValueTableVec = builder.CreateVector(tagValueTableOffsets);
    auto ncSrcSysTableVec = builder.CreateVector(srcSysTableOffsets);
    auto ncSrcSysIndicesVec = builder.CreateVector(srcSysIndices);
    auto ncDegradationVec = builder.CreateVector(degradation);

    auto nodeColumns = fbs::CreateNodeColumns(
        builder, logVolIdWidth, ncLogVolIdsU16Vec, ncLogVolIdsU32Vec, ncNameTableVec,
        nameIndexWidth, ncNameIndicesU16Vec, ncNameIndicesU32Vec, transformIndexWidth,
        ncTransformIndicesU16Vec, ncTransformIndicesU32Vec, ncParentIdsVec, ncTagCountsVec,
        ncTagKeyTableVec, ncTagRefsVec, ncTagValueTableVec, ncSrcSysTableVec, ncSrcSysIndicesVec,
        ncDegradationVec);

    auto tpRotationsVec = builder.CreateVector(transformPool.rotations);
    auto tpTranslationsVec = builder.CreateVector(transformPool.translations);
    auto tpTransformRotIndicesVec = builder.CreateVector(transformPool.transformRotIndices);
    auto tpTransformTrlIndicesVec = builder.CreateVector(transformPool.transformTrlIndices);
    auto transformPoolTable =
        fbs::CreateTransformPool(builder, tpRotationsVec, tpTranslationsVec,
                                 tpTransformRotIndicesVec, tpTransformTrlIndicesVec);

    // ── Root table ──────────────────────────────────────────────────────────
    auto sourceFileOff = builder.CreateSharedString(scene.sourceFile);
    auto logVolsVec = builder.CreateVector(lvOffsets);
    auto shapesVec = builder.CreateVector(shapeOffsets);
    auto matsVec = builder.CreateVector(matOffsets);

    const auto rootIt = nodeIdRemap.find(scene.rootId);
    if (rootIt == nodeIdRemap.end()) {
        throw std::runtime_error(
            std::format("root node id {} missing from node remap", scene.rootId.value));
    }

    return fbs::CreateSemanticScene(builder, rootIt->second, sourceFileOff, transformPoolTable,
                                    nodeColumns, logVolsVec, shapesVec, matsVec);
}

detail::SemanticScene semanticSceneFromFlatBuffer(const fbs::SemanticScene &fb) {
    detail::SemanticScene scene;
    scene.rootId = SemanticNodeId{fb.root_id()};
    if (fb.source_file()) {
        scene.sourceFile = fb.source_file()->str();
    }

    const auto *pool = fb.transforms();
    const auto *poolRots = pool ? pool->rotations() : nullptr;
    const auto *poolTrls = pool ? pool->translations() : nullptr;
    const auto *poolTfRotIdx = pool ? pool->transform_rot_indices() : nullptr;
    const auto *poolTfTrlIdx = pool ? pool->transform_trl_indices() : nullptr;

    // ── Shapes ──────────────────────────────────────────────────────────────
    if (fb.shapes()) {
        for (const auto *fbShape : *fb.shapes()) {
            SemanticShape shape;
            shape.id = SemanticShapeId{fbShape->id()};
            shape.data = deserializeShapeVariant(fbShape->data_type(), fbShape->data(), poolRots,
                                                 poolTrls, poolTfRotIdx, poolTfTrlIdx);
            scene.shapes[shape.id] = std::move(shape);
        }
    }

    // ── Materials ───────────────────────────────────────────────────────────
    if (fb.materials()) {
        for (const auto *fbMat : *fb.materials()) {
            SourceMaterial mat;
            mat.id = SemanticMaterialId{fbMat->id()};
            mat.name = fbMat->name() ? fbMat->name()->str() : "";
            mat.density = fbMat->density();
            if (fbMat->has_color()) {
                const auto *c = fbMat->color();
                mat.color = glm::vec3{c->x(), c->y(), c->z()};
            }
            scene.materials[mat.id] = std::move(mat);
        }
    }

    // ── Logical Volumes ─────────────────────────────────────────────────────
    if (fb.log_vols()) {
        for (const auto *fbLv : *fb.log_vols()) {
            SemanticLogicalVolume lv;
            lv.id = SemanticLogVolId{fbLv->id()};
            lv.name = fbLv->name() ? fbLv->name()->str() : "";
            lv.shapeId = SemanticShapeId{fbLv->shape_id()};
            lv.materialId = SemanticMaterialId{fbLv->material_id()};
            if (fbLv->daughters()) {
                lv.daughters.reserve(fbLv->daughters()->size());
                for (const auto *fbD : *fbLv->daughters()) {
                    SemanticDaughterPlacement d;
                    d.name = fbD->name() ? fbD->name()->str() : "";
                    d.logVolId = SemanticLogVolId{fbD->log_vol_id()};
                    d.localTransform = decodeTransform(poolRots, poolTrls, poolTfRotIdx,
                                                       poolTfTrlIdx, fbD->transform_index());
                    lv.daughters.push_back(std::move(d));
                }
            }
            scene.logVols[lv.id] = std::move(lv);
        }
    }

    // ── Nodes (column-oriented) ────────────────────────────────────────────
    if (const auto *nc = fb.nodes()) {
        const auto *logVolIdsU16 = nc->log_vol_ids_u16();
        const auto *logVolIdsU32 = nc->log_vol_ids_u32();
        const auto logVolIdWidth = nc->log_vol_id_width();
        const auto *nameTable = nc->name_table();
        const auto *nameIndicesU16 = nc->name_indices_u16();
        const auto *nameIndicesU32 = nc->name_indices_u32();
        const auto nameIndexWidth = nc->name_index_width();
        const auto *transformIndicesU16 = nc->transform_indices_u16();
        const auto *transformIndicesU32 = nc->transform_indices_u32();
        const auto transformIndexWidth = nc->transform_index_width();
        const auto *parentIdsVec = nc->parent_ids();
        const auto *tagCounts = nc->tag_counts();
        const auto *tkTable = nc->tag_key_table();
        const auto *tagRefs = nc->tag_refs();
        const auto *tvTable = nc->tag_value_table();
        const auto *ssTable = nc->source_system_table();
        const auto *ssIndices = nc->source_system_indices();
        const auto *degrad = nc->degradation();
        std::vector<SemanticNodeId> nodeOrder;

        std::size_t N = 0;
        if (parentIdsVec) {
            N = parentIdsVec->size();
        } else if (transformIndicesU32) {
            N = transformIndicesU32->size();
        } else if (transformIndicesU16) {
            N = transformIndicesU16->size();
        } else if (nameIndicesU32) {
            N = nameIndicesU32->size();
        } else if (nameIndicesU16) {
            N = nameIndicesU16->size();
        } else if (logVolIdsU32) {
            N = logVolIdsU32->size();
        } else if (logVolIdsU16) {
            N = logVolIdsU16->size();
        }

        if (N > 0) {
            nodeOrder.reserve(N);
            flatbuffers::uoffset_t tagCursor = 0;
            for (flatbuffers::uoffset_t i = 0; i < N; ++i) {
                SemanticNode node;
                node.id = SemanticNodeId{static_cast<uint64_t>(i) + 1u};
                nodeOrder.push_back(node.id);

                // Name from string table
                if (nameTable) {
                    uint32_t idx = 0;
                    if (nameIndexWidth == fbs::IndexWidth_U16) {
                        if (nameIndicesU16 && i < nameIndicesU16->size()) {
                            idx = nameIndicesU16->Get(i);
                        }
                    } else {
                        if (nameIndicesU32 && i < nameIndicesU32->size()) {
                            idx = nameIndicesU32->Get(i);
                        }
                    }
                    if (idx < nameTable->size()) {
                        auto *s = nameTable->Get(idx);
                        if (s) {
                            node.name = s->str();
                        }
                    }
                }

                uint32_t logVolId = 0;
                if (logVolIdWidth == fbs::LogVolIdWidth_U16) {
                    if (logVolIdsU16 && i < logVolIdsU16->size()) {
                        logVolId = logVolIdsU16->Get(i);
                    }
                } else {
                    if (logVolIdsU32 && i < logVolIdsU32->size()) {
                        logVolId = logVolIdsU32->Get(i);
                    }
                }
                node.logVolId = SemanticLogVolId{logVolId};

                node.localTransform = glm::dmat4{1.0};
                uint32_t transformIndex = 0;
                if (transformIndexWidth == fbs::IndexWidth_U16) {
                    if (transformIndicesU16 && i < transformIndicesU16->size()) {
                        transformIndex = transformIndicesU16->Get(i);
                    }
                } else {
                    if (transformIndicesU32 && i < transformIndicesU32->size()) {
                        transformIndex = transformIndicesU32->Get(i);
                    }
                }
                if (transformIndicesU16 || transformIndicesU32) {
                    node.localTransform = decodeTransform(poolRots, poolTrls, poolTfRotIdx,
                                                          poolTfTrlIdx, transformIndex);
                }

                // Parent
                if (parentIdsVec) {
                    auto pid = parentIdsVec->Get(i);
                    if (pid != 0) {
                        node.parentId = SemanticNodeId{pid};
                    }
                }

                // Tags (count + inline key/value ref pairs)
                if (tagCounts && tkTable && tagRefs && tvTable) {
                    const auto count = tagCounts->Get(i);
                    for (flatbuffers::uoffset_t j = 0; j < count; ++j) {
                        if (tagCursor >= tagRefs->size()) {
                            break;
                        }
                        const auto *tr = tagRefs->Get(tagCursor++);
                        if (!tr) {
                            continue;
                        }
                        auto ki = tr->key();
                        auto vi = tr->value();
                        if (ki < tkTable->size() && vi < tvTable->size()) {
                            auto *k = tkTable->Get(ki);
                            auto *v = tvTable->Get(vi);
                            if (k && v) {
                                node.tags[k->str()] = v->str();
                            }
                        }
                    }
                }

                // Source system
                if (ssTable && ssIndices) {
                    auto idx = ssIndices->Get(i);
                    if (idx < ssTable->size()) {
                        auto *s = ssTable->Get(idx);
                        if (s) {
                            node.sourceSystem = s->str();
                        }
                    }
                }

                // Degradation
                if (degrad) {
                    node.degradation.bits =
                        decltype(node.degradation.bits)(static_cast<unsigned long>(degrad->Get(i)));
                }

                scene.nodes[node.id] = std::move(node);
            }
        }

        // Reconstruct children lists from parent links.
        for (const auto id : nodeOrder) {
            const auto &node = scene.nodes.at(id);
            if (node.parentId && scene.nodes.contains(*node.parentId)) {
                scene.nodes.at(*node.parentId).children.push_back(id);
            }
        }
    }

    return scene;
}

SemanticFlatbufferSizeReport semanticFlatbufferSizeReport(const detail::SemanticScene &scene) {
    SemanticFlatbufferSizeReport report;
    report.nodeCount = scene.nodes.size();
    report.logicalVolumeCount = scene.logVols.size();
    report.shapeCount = scene.shapes.size();
    report.materialCount = scene.materials.size();

    TransformPoolBuild transformPool;
    std::size_t tagCount = 0;
    uint32_t maxNodeLogVolId = 0;
    std::unordered_set<std::string> uniqueNodeNames;
    uniqueNodeNames.reserve(scene.nodes.size());
    std::size_t uniqueNodeNameBytes = 0;

    // Mirror serialization: logical-volume IDs are remapped to dense [1..M].
    std::vector<SemanticLogVolId> orderedLogVolIds;
    orderedLogVolIds.reserve(scene.logVols.size());
    for (const auto &[id, lv] : scene.logVols) {
        (void)lv;
        orderedLogVolIds.push_back(id);
    }
    std::sort(orderedLogVolIds.begin(), orderedLogVolIds.end());
    std::unordered_map<SemanticLogVolId, uint32_t> logVolIdRemap;
    logVolIdRemap.reserve(orderedLogVolIds.size());
    for (std::size_t i = 0; i < orderedLogVolIds.size(); ++i) {
        logVolIdRemap.emplace(orderedLogVolIds[i], toU32Checked(i + 1, "logvol remap index"));
    }

    for (const auto &[id, node] : scene.nodes) {
        (void)id;
        transformPool.internTransform(node.localTransform);
        tagCount += node.tags.size();
        if (uniqueNodeNames.emplace(node.name).second) {
            // FlatBuffers strings are null-terminated UTF-8 payloads.
            uniqueNodeNameBytes += node.name.size() + 1;
        }
        const auto it = logVolIdRemap.find(node.logVolId);
        if (it == logVolIdRemap.end()) {
            throw std::runtime_error(std::format("node logvol {} missing from logical volume table",
                                                 node.logVolId.value));
        }
        maxNodeLogVolId = std::max(maxNodeLogVolId, it->second);
    }

    for (const auto &[id, lv] : scene.logVols) {
        (void)id;
        report.daughterPlacementCount += lv.daughters.size();
        for (const auto &d : lv.daughters) {
            transformPool.internTransform(d.localTransform);
        }
    }

    for (const auto &[id, shape] : scene.shapes) {
        (void)id;
        std::visit(detail::overloaded{
                       [&](const BooleanUnion &s) {
                           ++report.booleanShapeCount;
                           transformPool.internTransform(s.rightTransform);
                       },
                       [&](const BooleanIntersection &s) {
                           ++report.booleanShapeCount;
                           transformPool.internTransform(s.rightTransform);
                       },
                       [&](const BooleanSubtraction &s) {
                           ++report.booleanShapeCount;
                           transformPool.internTransform(s.rightTransform);
                       },
                       [&](const auto &) {},
                   },
                   shape.data);
    }

    report.uniqueRotationCount = transformPool.rotations.size() / 9;
    report.uniqueTranslationCount = transformPool.translations.size() / 3;
    report.uniqueTransformCount = transformPool.transformRotIndices.size();
    report.nodeLogVolIdsUseU16 = maxNodeLogVolId <= std::numeric_limits<uint16_t>::max();
    report.nodeNameIndicesUseU16 = uniqueNodeNames.size() <= std::numeric_limits<uint16_t>::max();
    report.nodeTransformIndicesUseU16 =
        report.uniqueTransformCount <= std::numeric_limits<uint16_t>::max();

    auto push = [&](std::string label, std::size_t bytes) {
        report.entries.push_back({std::move(label), bytes});
        report.estimatedVectorPayloadBytes += bytes;
    };

    // NodeColumns vectors
    push(report.nodeLogVolIdsUseU16 ? "nodes.log_vol_ids_u16" : "nodes.log_vol_ids_u32",
         report.nodeCount * (report.nodeLogVolIdsUseU16 ? sizeof(uint16_t) : sizeof(uint32_t)));
    push("nodes.name_table_offsets", uniqueNodeNames.size() * sizeof(uint32_t));
    push("nodes.name_table_strings", uniqueNodeNameBytes);
    push(report.nodeNameIndicesUseU16 ? "nodes.name_indices_u16" : "nodes.name_indices_u32",
         report.nodeCount * (report.nodeNameIndicesUseU16 ? sizeof(uint16_t) : sizeof(uint32_t)));
    push(report.nodeTransformIndicesUseU16 ? "nodes.transform_indices_u16"
                                           : "nodes.transform_indices_u32",
         report.nodeCount *
             (report.nodeTransformIndicesUseU16 ? sizeof(uint16_t) : sizeof(uint32_t)));
    push("nodes.parent_ids", report.nodeCount * sizeof(uint32_t));
    push("nodes.tag_counts", report.nodeCount * sizeof(uint8_t));
    push("nodes.tag_refs", tagCount * 2 * sizeof(uint8_t));
    push("nodes.source_system_indices", report.nodeCount * sizeof(uint8_t));
    push("nodes.degradation", report.nodeCount * sizeof(uint8_t));

    // TransformPool vectors
    push("transforms.rotations", transformPool.rotations.size() * sizeof(double));
    push("transforms.translations", transformPool.translations.size() * sizeof(double));
    push("transforms.transform_rot_indices",
         transformPool.transformRotIndices.size() * sizeof(uint32_t));
    push("transforms.transform_trl_indices",
         transformPool.transformTrlIndices.size() * sizeof(uint32_t));

    // High-frequency scalar fields outside columns
    push("log_vols.id", report.logicalVolumeCount * sizeof(uint32_t));
    push("log_vols.shape_id", report.logicalVolumeCount * sizeof(uint32_t));
    push("log_vols.material_id", report.logicalVolumeCount * sizeof(uint32_t));
    push("daughter.log_vol_id", report.daughterPlacementCount * sizeof(uint32_t));
    push("daughter.transform_index", report.daughterPlacementCount * sizeof(uint32_t));
    push("shapes.id", report.shapeCount * sizeof(uint32_t));
    push("bool.right_transform_index", report.booleanShapeCount * sizeof(uint32_t));
    push("materials.id", report.materialCount * sizeof(uint32_t));

    return report;
}

std::string formatSemanticFlatbufferSizeReport(const SemanticFlatbufferSizeReport &report) {
    std::vector<SemanticFlatbufferSizeEntry> entries = report.entries;
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.bytes > b.bytes; });

    std::ostringstream os;
    os << std::format(
        "semantic flatbuffer size report\n"
        "  nodes={} log_vols={} daughters={} shapes={} (boolean={}) materials={}\n"
        "  unique transforms: {} rows (rotations={}, translations={})\n"
        "  logical-volume ids: remapped to dense local ids\n"
        "  node log_vol_ids width: {}\n"
        "  node name_indices width: {}\n"
        "  node transform_indices width: {}\n"
        "  node ids: remapped to row order (ids column omitted)\n"
        "  topology storage: parent_ids only (children reconstructed on load)\n"
        "  estimated vector payload: {} ({})\n",
        report.nodeCount, report.logicalVolumeCount, report.daughterPlacementCount,
        report.shapeCount, report.booleanShapeCount, report.materialCount,
        report.uniqueTransformCount, report.uniqueRotationCount, report.uniqueTranslationCount,
        report.nodeLogVolIdsUseU16 ? "u16" : "u32", report.nodeNameIndicesUseU16 ? "u16" : "u32",
        report.nodeTransformIndicesUseU16 ? "u16" : "u32", report.estimatedVectorPayloadBytes,
        formatBytes(report.estimatedVectorPayloadBytes));

    for (const auto &e : entries) {
        os << std::format("    {:35} {:>12} ({})\n", e.label, e.bytes, formatBytes(e.bytes));
    }
    return os.str();
}

// ── Layer 2: Byte buffer convenience ────────────────────────────────────────

std::vector<std::byte> semanticSceneToBytes(const detail::SemanticScene &scene) {
    flatbuffers::FlatBufferBuilder builder{1024};
    auto root = semanticSceneToFlatBuffer(builder, scene);
    fbs::FinishSemanticSceneBuffer(builder, root);
    auto *ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    auto span = std::as_bytes(std::span{ptr, size});
    return std::vector<std::byte>(span.begin(), span.end());
}

detail::SemanticScene semanticSceneFromBytes(std::span<const std::byte> buf) {
    const auto *ptr = reinterpret_cast<const uint8_t *>(buf.data());
    flatbuffers::Verifier verifier{ptr, buf.size()};
    if (!fbs::VerifySemanticSceneBuffer(verifier)) {
        throw std::runtime_error("FlatBuffer verification failed: invalid buffer");
    }
    const auto *fb = fbs::GetSemanticScene(ptr);
    return semanticSceneFromFlatBuffer(*fb);
}

} // namespace nodehammer
