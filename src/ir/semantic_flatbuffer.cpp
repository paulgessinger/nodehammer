#include <nodehammer/ir/semantic_flatbuffer.hpp>

#include <nodehammer/detail/overloaded.hpp>

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace nodehammer {

namespace {

// ── Mat4x4 helpers ──────────────────────────────────────────────────────────

bool isIdentityRotation(const glm::dmat4 &m) {
    return m[0][0] == 1.0 && m[0][1] == 0.0 && m[0][2] == 0.0 && m[1][0] == 0.0 && m[1][1] == 1.0 &&
           m[1][2] == 0.0 && m[2][0] == 0.0 && m[2][1] == 0.0 && m[2][2] == 1.0;
}

bool isZeroTranslation(const glm::dmat4 &m) {
    return m[3][0] == 0.0 && m[3][1] == 0.0 && m[3][2] == 0.0;
}

flatbuffers::Offset<flatbuffers::Vector<double>>
serializeRotation(flatbuffers::FlatBufferBuilder &builder, const glm::dmat4 &m) {
    if (isIdentityRotation(m)) {
        return {};
    }
    std::vector<double> r(9);
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            r[static_cast<std::size_t>(col * 3 + row)] = m[col][row];
        }
    }
    return builder.CreateVector(r);
}

flatbuffers::Offset<flatbuffers::Vector<double>>
serializeTranslation(flatbuffers::FlatBufferBuilder &builder, const glm::dmat4 &m) {
    if (isZeroTranslation(m)) {
        return {};
    }
    std::vector<double> t = {m[3][0], m[3][1], m[3][2]};
    return builder.CreateVector(t);
}

glm::dmat4 deserializeRotTrl(const flatbuffers::Vector<double> *rot,
                             const flatbuffers::Vector<double> *trl) {
    glm::dmat4 m{1.0};
    if (rot && rot->size() == 9) {
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                m[col][row] = rot->Get(static_cast<flatbuffers::uoffset_t>(col * 3 + row));
            }
        }
    }
    if (trl && trl->size() == 3) {
        m[3][0] = trl->Get(0);
        m[3][1] = trl->Get(1);
        m[3][2] = trl->Get(2);
    }
    return m;
}

// ── Shape serialization ─────────────────────────────────────────────────────

struct ShapeOffsetResult {
    fbs::ShapeData type;
    flatbuffers::Offset<void> offset;
};

ShapeOffsetResult serializeShapeVariant(flatbuffers::FlatBufferBuilder &builder,
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
                auto rot = serializeRotation(builder, s.rightTransform);
                auto trl = serializeTranslation(builder, s.rightTransform);
                auto o = fbs::CreateBooleanUnion(builder, s.left.value, s.right.value, rot, trl);
                return {fbs::ShapeData_BooleanUnion, o.Union()};
            },
            [&](const BooleanIntersection &s) -> ShapeOffsetResult {
                auto rot = serializeRotation(builder, s.rightTransform);
                auto trl = serializeTranslation(builder, s.rightTransform);
                auto o =
                    fbs::CreateBooleanIntersection(builder, s.left.value, s.right.value, rot, trl);
                return {fbs::ShapeData_BooleanIntersection, o.Union()};
            },
            [&](const BooleanSubtraction &s) -> ShapeOffsetResult {
                auto rot = serializeRotation(builder, s.rightTransform);
                auto trl = serializeTranslation(builder, s.rightTransform);
                auto o =
                    fbs::CreateBooleanSubtraction(builder, s.left.value, s.right.value, rot, trl);
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

SemanticShapeVariant deserializeShapeVariant(fbs::ShapeData type, const void *data) {
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
        result.rightTransform = deserializeRotTrl(s->right_rotation(), s->right_translation());
        return result;
    }
    case fbs::ShapeData_BooleanIntersection: {
        const auto *s = static_cast<const fbs::BooleanIntersection *>(data);
        BooleanIntersection result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = deserializeRotTrl(s->right_rotation(), s->right_translation());
        return result;
    }
    case fbs::ShapeData_BooleanSubtraction: {
        const auto *s = static_cast<const fbs::BooleanSubtraction *>(data);
        BooleanSubtraction result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = deserializeRotTrl(s->right_rotation(), s->right_translation());
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
semanticSceneToFlatBuffer(flatbuffers::FlatBufferBuilder &builder, const SemanticScene &scene) {

    // ── Shapes ──────────────────────────────────────────────────────────────
    std::vector<flatbuffers::Offset<fbs::Shape>> shapeOffsets;
    shapeOffsets.reserve(scene.shapes.size());
    for (const auto &[id, shape] : scene.shapes) {
        auto [dataType, dataOffset] = serializeShapeVariant(builder, shape.data);
        auto o = fbs::CreateShape(builder, shape.id.value, dataType, dataOffset);
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
        auto o = fbs::CreateMaterial(builder, mat.id.value, nameOff, hasColor, &color, mat.density);
        matOffsets.push_back(o);
    }

    // ── Logical Volumes ─────────────────────────────────────────────────────
    std::vector<flatbuffers::Offset<fbs::LogicalVolume>> lvOffsets;
    lvOffsets.reserve(scene.logVols.size());
    for (const auto &[id, lv] : scene.logVols) {
        auto nameOff = builder.CreateSharedString(lv.name);

        std::vector<flatbuffers::Offset<fbs::DaughterPlacement>> daughterOffsets;
        daughterOffsets.reserve(lv.daughters.size());
        for (const auto &d : lv.daughters) {
            auto dNameOff = builder.CreateSharedString(d.name);
            auto dRot = serializeRotation(builder, d.localTransform);
            auto dTrl = serializeTranslation(builder, d.localTransform);
            auto dOff =
                fbs::CreateDaughterPlacement(builder, dNameOff, d.logVolId.value, dRot, dTrl);
            daughterOffsets.push_back(dOff);
        }

        auto daughtersVec =
            daughterOffsets.empty()
                ? flatbuffers::Offset<
                      flatbuffers::Vector<flatbuffers::Offset<fbs::DaughterPlacement>>>{}
                : builder.CreateVector(daughterOffsets);

        auto o = fbs::CreateLogicalVolume(builder, lv.id.value, nameOff, lv.shapeId.value,
                                          lv.materialId.value, daughtersVec);
        lvOffsets.push_back(o);
    }

    // ── Nodes (column-oriented) ────────────────────────────────────────────
    // Collect nodes into a stable order (sorted by ID for reproducibility).
    std::vector<const SemanticNode *> orderedNodes;
    orderedNodes.reserve(scene.nodes.size());
    for (const auto &[id, node] : scene.nodes) {
        orderedNodes.push_back(&node);
    }
    std::sort(orderedNodes.begin(), orderedNodes.end(),
              [](const SemanticNode *a, const SemanticNode *b) { return a->id < b->id; });

    const auto N = orderedNodes.size();

    // Parallel arrays
    std::vector<uint64_t> nodeIds(N);
    std::vector<uint64_t> logVolIds(N);
    std::vector<uint32_t> rotIndices(N);
    std::vector<double> rotations; // flattened 3x3 rotation matrices (9 doubles each)
    std::map<std::array<uint64_t, 9>, uint32_t> rotDedup;
    std::vector<uint32_t> trlIndices(N);
    std::vector<double> translations; // flattened translations (3 doubles each)
    std::map<std::array<uint64_t, 3>, uint32_t> trlDedup;
    std::vector<uint64_t> parentIds(N);
    std::vector<uint32_t> childrenOffsets;
    std::vector<uint64_t> childrenData;
    std::vector<uint32_t> tagOffsets;
    std::vector<uint16_t> tagKeyIndices;
    std::vector<uint16_t> tagValueIndices;
    std::vector<uint32_t> degradation(N);

    childrenOffsets.reserve(N + 1);
    tagOffsets.reserve(N + 1);

    // String tables
    std::unordered_map<std::string, uint32_t> nameMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> nameTableOffsets;
    std::vector<uint32_t> nameIndices(N);

    std::unordered_map<std::string, uint16_t> tagKeyMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> tagKeyTableOffsets;
    std::unordered_map<std::string, uint16_t> tagValueMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> tagValueTableOffsets;

    std::unordered_map<std::string, uint8_t> srcSysMap;
    std::vector<flatbuffers::Offset<flatbuffers::String>> srcSysTableOffsets;
    std::vector<uint8_t> srcSysIndices(N);

    static constexpr uint32_t kSentinel = 0xFFFFFFFF;

    auto internName = [&](const std::string &s) -> uint32_t {
        auto [it, inserted] =
            nameMap.try_emplace(s, static_cast<uint32_t>(nameTableOffsets.size()));
        if (inserted) {
            nameTableOffsets.push_back(builder.CreateSharedString(s));
        }
        return it->second;
    };

    auto internTagKey = [&](const std::string &s) -> uint16_t {
        auto [it, inserted] =
            tagKeyMap.try_emplace(s, static_cast<uint16_t>(tagKeyTableOffsets.size()));
        if (inserted) {
            tagKeyTableOffsets.push_back(builder.CreateSharedString(s));
        }
        return it->second;
    };

    auto internTagValue = [&](const std::string &s) -> uint16_t {
        auto [it, inserted] =
            tagValueMap.try_emplace(s, static_cast<uint16_t>(tagValueTableOffsets.size()));
        if (inserted) {
            tagValueTableOffsets.push_back(builder.CreateSharedString(s));
        }
        return it->second;
    };

    auto internSrcSys = [&](const std::string &s) -> uint8_t {
        auto [it, inserted] =
            srcSysMap.try_emplace(s, static_cast<uint8_t>(srcSysTableOffsets.size()));
        if (inserted) {
            srcSysTableOffsets.push_back(builder.CreateSharedString(s));
        }
        return it->second;
    };

    for (std::size_t i = 0; i < N; ++i) {
        const auto &node = *orderedNodes[i];

        nodeIds[i] = node.id.value;
        nameIndices[i] = internName(node.name);
        logVolIds[i] = node.logVolId.value;

        // Rotation (3x3 upper-left of dmat4), deduplicated
        const auto &m = node.localTransform;
        bool identityRot = m[0][0] == 1.0 && m[0][1] == 0.0 && m[0][2] == 0.0 && m[1][0] == 0.0 &&
                           m[1][1] == 1.0 && m[1][2] == 0.0 && m[2][0] == 0.0 && m[2][1] == 0.0 &&
                           m[2][2] == 1.0;
        if (identityRot) {
            rotIndices[i] = kSentinel;
        } else {
            std::array<uint64_t, 9> key;
            for (int col = 0; col < 3; ++col) {
                for (int row = 0; row < 3; ++row) {
                    key[static_cast<std::size_t>(col * 3 + row)] =
                        std::bit_cast<uint64_t>(m[col][row]);
                }
            }
            auto [it, inserted] =
                rotDedup.try_emplace(key, static_cast<uint32_t>(rotations.size() / 9));
            if (inserted) {
                for (int col = 0; col < 3; ++col) {
                    for (int row = 0; row < 3; ++row) {
                        rotations.push_back(m[col][row]);
                    }
                }
            }
            rotIndices[i] = it->second;
        }

        // Translation, deduplicated
        bool zeroTrl = m[3][0] == 0.0 && m[3][1] == 0.0 && m[3][2] == 0.0;
        if (zeroTrl) {
            trlIndices[i] = kSentinel;
        } else {
            std::array<uint64_t, 3> key{
                std::bit_cast<uint64_t>(m[3][0]),
                std::bit_cast<uint64_t>(m[3][1]),
                std::bit_cast<uint64_t>(m[3][2]),
            };
            auto [it, inserted] =
                trlDedup.try_emplace(key, static_cast<uint32_t>(translations.size() / 3));
            if (inserted) {
                translations.push_back(m[3][0]);
                translations.push_back(m[3][1]);
                translations.push_back(m[3][2]);
            }
            trlIndices[i] = it->second;
        }

        // Parent
        parentIds[i] = node.parentId ? node.parentId->value : 0;

        // Children CSR
        childrenOffsets.push_back(static_cast<uint32_t>(childrenData.size()));
        for (const auto &childId : node.children) {
            childrenData.push_back(childId.value);
        }

        // Tags CSR with string table indices
        tagOffsets.push_back(static_cast<uint32_t>(tagKeyIndices.size()));
        for (const auto &[k, v] : node.tags) {
            tagKeyIndices.push_back(internTagKey(k));
            tagValueIndices.push_back(internTagValue(v));
        }

        srcSysIndices[i] = internSrcSys(node.sourceSystem);

        degradation[i] = static_cast<uint32_t>(node.degradation.bits.to_ulong());
    }
    // CSR sentinels
    childrenOffsets.push_back(static_cast<uint32_t>(childrenData.size()));
    tagOffsets.push_back(static_cast<uint32_t>(tagKeyIndices.size()));

    // Build NodeColumns
    auto ncIdsVec = builder.CreateVector(nodeIds);
    auto ncLogVolIdsVec = builder.CreateVector(logVolIds);
    auto ncNameTableVec = builder.CreateVector(nameTableOffsets);
    auto ncNameIndicesVec = builder.CreateVector(nameIndices);
    auto ncRotIndicesVec = builder.CreateVector(rotIndices);
    auto ncRotationsVec = builder.CreateVector(rotations);
    auto ncTrlIndicesVec = builder.CreateVector(trlIndices);
    auto ncTranslationsVec = builder.CreateVector(translations);
    auto ncParentIdsVec = builder.CreateVector(parentIds);
    auto ncChildrenOffsetsVec = builder.CreateVector(childrenOffsets);
    auto ncChildrenDataVec = builder.CreateVector(childrenData);
    auto ncTagOffsetsVec = builder.CreateVector(tagOffsets);
    auto ncTagKeyTableVec = builder.CreateVector(tagKeyTableOffsets);
    auto ncTagKeyIndicesVec = builder.CreateVector(tagKeyIndices);
    auto ncTagValueTableVec = builder.CreateVector(tagValueTableOffsets);
    auto ncTagValueIndicesVec = builder.CreateVector(tagValueIndices);
    auto ncSrcSysTableVec = builder.CreateVector(srcSysTableOffsets);
    auto ncSrcSysIndicesVec = builder.CreateVector(srcSysIndices);
    auto ncDegradationVec = builder.CreateVector(degradation);

    auto nodeColumns = fbs::CreateNodeColumns(
        builder, ncIdsVec, ncLogVolIdsVec, ncNameTableVec, ncNameIndicesVec, ncRotIndicesVec,
        ncRotationsVec, ncTrlIndicesVec, ncTranslationsVec, ncParentIdsVec, ncChildrenOffsetsVec,
        ncChildrenDataVec, ncTagOffsetsVec, ncTagKeyTableVec, ncTagKeyIndicesVec,
        ncTagValueTableVec, ncTagValueIndicesVec, ncSrcSysTableVec, ncSrcSysIndicesVec,
        ncDegradationVec);

    // ── Root table ──────────────────────────────────────────────────────────
    auto sourceFileOff = builder.CreateSharedString(scene.sourceFile);
    auto logVolsVec = builder.CreateVector(lvOffsets);
    auto shapesVec = builder.CreateVector(shapeOffsets);
    auto matsVec = builder.CreateVector(matOffsets);

    return fbs::CreateSemanticScene(builder, scene.rootId.value, sourceFileOff, nodeColumns,
                                    logVolsVec, shapesVec, matsVec);
}

SemanticScene semanticSceneFromFlatBuffer(const fbs::SemanticScene &fb) {
    SemanticScene scene;
    scene.rootId = SemanticNodeId{fb.root_id()};
    if (fb.source_file()) {
        scene.sourceFile = fb.source_file()->str();
    }

    // ── Shapes ──────────────────────────────────────────────────────────────
    if (fb.shapes()) {
        for (const auto *fbShape : *fb.shapes()) {
            SemanticShape shape;
            shape.id = SemanticShapeId{fbShape->id()};
            shape.data = deserializeShapeVariant(fbShape->data_type(), fbShape->data());
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
                    d.localTransform = deserializeRotTrl(fbD->rotation(), fbD->translation());
                    lv.daughters.push_back(std::move(d));
                }
            }
            scene.logVols[lv.id] = std::move(lv);
        }
    }

    // ── Nodes (column-oriented) ────────────────────────────────────────────
    if (const auto *nc = fb.nodes()) {
        const auto *ids = nc->ids();
        const auto *logVolIds = nc->log_vol_ids();
        const auto *nameTable = nc->name_table();
        const auto *nameIndices = nc->name_indices();
        const auto *rotIdx = nc->rot_indices();
        const auto *rots = nc->rotations();
        const auto *trlIdx = nc->trl_indices();
        const auto *trls = nc->translations();
        const auto *parentIdsVec = nc->parent_ids();
        const auto *cOffsets = nc->children_offsets();
        const auto *cData = nc->children_data();
        const auto *tOffsets = nc->tag_offsets();
        const auto *tkTable = nc->tag_key_table();
        const auto *tkIndices = nc->tag_key_indices();
        const auto *tvTable = nc->tag_value_table();
        const auto *tvIndices = nc->tag_value_indices();
        const auto *ssTable = nc->source_system_table();
        const auto *ssIndices = nc->source_system_indices();
        const auto *degrad = nc->degradation();

        if (ids) {
            const auto N = ids->size();
            for (flatbuffers::uoffset_t i = 0; i < N; ++i) {
                SemanticNode node;
                node.id = SemanticNodeId{ids->Get(i)};

                // Name from string table
                if (nameTable && nameIndices) {
                    auto idx = nameIndices->Get(i);
                    if (idx < nameTable->size()) {
                        auto *s = nameTable->Get(idx);
                        if (s) {
                            node.name = s->str();
                        }
                    }
                }

                node.logVolId = SemanticLogVolId{logVolIds ? logVolIds->Get(i) : 0};

                // Rotation + Translation
                static constexpr uint32_t kIdentity = 0xFFFFFFFF;
                node.localTransform = glm::dmat4{1.0};
                if (rotIdx && rots) {
                    auto ri = rotIdx->Get(i);
                    if (ri != kIdentity) {
                        auto base = static_cast<flatbuffers::uoffset_t>(ri * 9);
                        auto &m = node.localTransform;
                        for (flatbuffers::uoffset_t col = 0; col < 3; ++col) {
                            for (flatbuffers::uoffset_t row = 0; row < 3; ++row) {
                                m[static_cast<int>(col)][static_cast<int>(row)] =
                                    rots->Get(base + col * 3 + row);
                            }
                        }
                    }
                }
                if (trlIdx && trls) {
                    auto ti = trlIdx->Get(i);
                    if (ti != kIdentity) {
                        auto base = static_cast<flatbuffers::uoffset_t>(ti * 3);
                        node.localTransform[3][0] = trls->Get(base);
                        node.localTransform[3][1] = trls->Get(base + 1);
                        node.localTransform[3][2] = trls->Get(base + 2);
                    }
                }

                // Parent
                if (parentIdsVec) {
                    auto pid = parentIdsVec->Get(i);
                    if (pid != 0) {
                        node.parentId = SemanticNodeId{pid};
                    }
                }

                // Children (CSR)
                if (cOffsets && cData) {
                    auto begin = cOffsets->Get(i);
                    auto end = cOffsets->Get(i + 1);
                    node.children.reserve(end - begin);
                    for (auto j = begin; j < end; ++j) {
                        node.children.push_back(SemanticNodeId{cData->Get(j)});
                    }
                }

                // Tags (CSR with string tables)
                if (tOffsets && tkTable && tkIndices && tvTable && tvIndices) {
                    auto begin = tOffsets->Get(i);
                    auto end = tOffsets->Get(i + 1);
                    for (auto j = begin; j < end; ++j) {
                        auto ki = tkIndices->Get(j);
                        auto vi = tvIndices->Get(j);
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
    }

    return scene;
}

// ── Layer 2: Byte buffer convenience ────────────────────────────────────────

std::vector<std::byte> semanticSceneToBytes(const SemanticScene &scene) {
    flatbuffers::FlatBufferBuilder builder{1024};
    auto root = semanticSceneToFlatBuffer(builder, scene);
    fbs::FinishSemanticSceneBuffer(builder, root);
    auto *ptr = builder.GetBufferPointer();
    auto size = builder.GetSize();
    auto span = std::as_bytes(std::span{ptr, size});
    return std::vector<std::byte>(span.begin(), span.end());
}

SemanticScene semanticSceneFromBytes(std::span<const std::byte> buf) {
    const auto *ptr = reinterpret_cast<const uint8_t *>(buf.data());
    flatbuffers::Verifier verifier{ptr, buf.size()};
    if (!fbs::VerifySemanticSceneBuffer(verifier)) {
        throw std::runtime_error("FlatBuffer verification failed: invalid buffer");
    }
    const auto *fb = fbs::GetSemanticScene(ptr);
    return semanticSceneFromFlatBuffer(*fb);
}

} // namespace nodehammer
