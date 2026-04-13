#include <nodehammer/ir/semantic_flatbuffer.hpp>

#include <nodehammer/detail/overloaded.hpp>

#include <flatbuffers/flatbuffers.h>

#include <format>
#include <stdexcept>

namespace nodehammer {

namespace {

// ── Mat4x4 helpers ──────────────────────────────────────────────────────────

bool isIdentity(const glm::dmat4 &m) {
    // GLM identity: diagonal = 1, rest = 0
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            if (m[c][r] != (c == r ? 1.0 : 0.0)) {
                return false;
            }
        }
    }
    return true;
}

fbs::Mat4x4 toFbsMat4(const glm::dmat4 &m) {
    // Column-major: m[col][row]
    return fbs::Mat4x4{
        m[0][0], m[0][1], m[0][2], m[0][3], // col 0
        m[1][0], m[1][1], m[1][2], m[1][3], // col 1
        m[2][0], m[2][1], m[2][2], m[2][3], // col 2
        m[3][0], m[3][1], m[3][2], m[3][3], // col 3
    };
}

glm::dmat4 fromFbsMat4(const fbs::Mat4x4 *mat) {
    if (mat == nullptr) {
        return glm::dmat4{1.0};
    }
    glm::dmat4 m;
    m[0][0] = mat->m00();
    m[0][1] = mat->m10();
    m[0][2] = mat->m20();
    m[0][3] = mat->m30();
    m[1][0] = mat->m01();
    m[1][1] = mat->m11();
    m[1][2] = mat->m21();
    m[1][3] = mat->m31();
    m[2][0] = mat->m02();
    m[2][1] = mat->m12();
    m[2][2] = mat->m22();
    m[2][3] = mat->m32();
    m[3][0] = mat->m03();
    m[3][1] = mat->m13();
    m[3][2] = mat->m23();
    m[3][3] = mat->m33();
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
                const fbs::Mat4x4 *matPtr = nullptr;
                fbs::Mat4x4 mat;
                if (!isIdentity(s.rightTransform)) {
                    mat = toFbsMat4(s.rightTransform);
                    matPtr = &mat;
                }
                auto o = fbs::CreateBooleanUnion(builder, s.left.value, s.right.value, matPtr);
                return {fbs::ShapeData_BooleanUnion, o.Union()};
            },
            [&](const BooleanIntersection &s) -> ShapeOffsetResult {
                const fbs::Mat4x4 *matPtr = nullptr;
                fbs::Mat4x4 mat;
                if (!isIdentity(s.rightTransform)) {
                    mat = toFbsMat4(s.rightTransform);
                    matPtr = &mat;
                }
                auto o =
                    fbs::CreateBooleanIntersection(builder, s.left.value, s.right.value, matPtr);
                return {fbs::ShapeData_BooleanIntersection, o.Union()};
            },
            [&](const BooleanSubtraction &s) -> ShapeOffsetResult {
                const fbs::Mat4x4 *matPtr = nullptr;
                fbs::Mat4x4 mat;
                if (!isIdentity(s.rightTransform)) {
                    mat = toFbsMat4(s.rightTransform);
                    matPtr = &mat;
                }
                auto o =
                    fbs::CreateBooleanSubtraction(builder, s.left.value, s.right.value, matPtr);
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
        result.rightTransform = fromFbsMat4(s->right_transform());
        return result;
    }
    case fbs::ShapeData_BooleanIntersection: {
        const auto *s = static_cast<const fbs::BooleanIntersection *>(data);
        BooleanIntersection result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = fromFbsMat4(s->right_transform());
        return result;
    }
    case fbs::ShapeData_BooleanSubtraction: {
        const auto *s = static_cast<const fbs::BooleanSubtraction *>(data);
        BooleanSubtraction result;
        result.left = SemanticShapeId{s->left()};
        result.right = SemanticShapeId{s->right()};
        result.rightTransform = fromFbsMat4(s->right_transform());
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
            const fbs::Mat4x4 *matPtr = nullptr;
            fbs::Mat4x4 mat;
            if (!isIdentity(d.localTransform)) {
                mat = toFbsMat4(d.localTransform);
                matPtr = &mat;
            }
            auto dOff = fbs::CreateDaughterPlacement(builder, dNameOff, d.logVolId.value, matPtr);
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

    // ── Nodes ───────────────────────────────────────────────────────────────
    std::vector<flatbuffers::Offset<fbs::Node>> nodeOffsets;
    nodeOffsets.reserve(scene.nodes.size());
    for (const auto &[id, node] : scene.nodes) {
        auto nameOff = builder.CreateSharedString(node.name);

        auto childrenVec =
            node.children.empty()
                ? flatbuffers::Offset<flatbuffers::Vector<uint64_t>>{}
                : builder.CreateVector(reinterpret_cast<const uint64_t *>(node.children.data()),
                                       node.children.size());

        // originalPath is NOT serialized — it is recomputed after loading
        // (same as JSON and worldTransform).

        std::vector<flatbuffers::Offset<fbs::StringPair>> tagOffsets;
        tagOffsets.reserve(node.tags.size());
        for (const auto &[k, v] : node.tags) {
            tagOffsets.push_back(fbs::CreateStringPair(builder, builder.CreateSharedString(k),
                                                       builder.CreateSharedString(v)));
        }
        auto tagsVec =
            tagOffsets.empty()
                ? flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<fbs::StringPair>>>{}
                : builder.CreateVector(tagOffsets);

        // Only create string offsets for non-empty strings
        auto sourceSystemOff = node.sourceSystem.empty()
                                   ? flatbuffers::Offset<flatbuffers::String>{}
                                   : builder.CreateSharedString(node.sourceSystem);

        const fbs::Mat4x4 *matPtr = nullptr;
        fbs::Mat4x4 mat;
        if (!isIdentity(node.localTransform)) {
            mat = toFbsMat4(node.localTransform);
            matPtr = &mat;
        }

        // Use the builder API to omit default-valued fields
        fbs::NodeBuilder nb{builder};
        nb.add_id(node.id.value);
        nb.add_name(nameOff);
        nb.add_log_vol_id(node.logVolId.value);
        if (matPtr != nullptr) {
            nb.add_local_transform(matPtr);
        }
        if (node.parentId.has_value()) {
            nb.add_has_parent(true);
            nb.add_parent_id(node.parentId->value);
        }
        if (!node.children.empty()) {
            nb.add_children(childrenVec);
        }
        if (!node.tags.empty()) {
            nb.add_tags(tagsVec);
        }
        if (!node.sourceSystem.empty()) {
            nb.add_source_system(sourceSystemOff);
        }
        if (node.degradation.bits.any()) {
            nb.add_degradation(static_cast<uint32_t>(node.degradation.bits.to_ulong()));
        }
        nodeOffsets.push_back(nb.Finish());
    }

    // ── Root table ──────────────────────────────────────────────────────────
    auto sourceFileOff = builder.CreateSharedString(scene.sourceFile);
    auto nodesVec = builder.CreateVector(nodeOffsets);
    auto logVolsVec = builder.CreateVector(lvOffsets);
    auto shapesVec = builder.CreateVector(shapeOffsets);
    auto matsVec = builder.CreateVector(matOffsets);

    return fbs::CreateSemanticScene(builder, scene.rootId.value, sourceFileOff, nodesVec,
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
                    d.localTransform = fromFbsMat4(fbD->local_transform());
                    lv.daughters.push_back(std::move(d));
                }
            }
            scene.logVols[lv.id] = std::move(lv);
        }
    }

    // ── Nodes ───────────────────────────────────────────────────────────────
    if (fb.nodes()) {
        for (const auto *fbNode : *fb.nodes()) {
            SemanticNode node;
            node.id = SemanticNodeId{fbNode->id()};
            node.name = fbNode->name() ? fbNode->name()->str() : "";
            node.logVolId = SemanticLogVolId{fbNode->log_vol_id()};
            node.localTransform = fromFbsMat4(fbNode->local_transform());
            if (fbNode->has_parent()) {
                node.parentId = SemanticNodeId{fbNode->parent_id()};
            }
            if (fbNode->children()) {
                node.children.reserve(fbNode->children()->size());
                for (auto childId : *fbNode->children()) {
                    node.children.push_back(SemanticNodeId{childId});
                }
            }
            // originalPath is recomputed after loading (not serialized).
            if (fbNode->tags()) {
                for (const auto *pair : *fbNode->tags()) {
                    if (pair->key() && pair->value()) {
                        node.tags[pair->key()->str()] = pair->value()->str();
                    }
                }
            }
            node.sourceSystem = fbNode->source_system() ? fbNode->source_system()->str() : "";
            node.degradation.bits =
                decltype(node.degradation.bits)(static_cast<unsigned long>(fbNode->degradation()));
            scene.nodes[node.id] = std::move(node);
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
