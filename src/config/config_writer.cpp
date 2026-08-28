#include <config/config_enums.hpp>
#include <config/config_writer.hpp>
#include <detail/overloaded.hpp>

#include <toml++/toml.hpp>

#include <sstream>

namespace nodehammer::config {

using detail::overloaded;

namespace {

// ── Helpers ─────────────────────────────────────────────────────────────────

toml::array colorToArray(const Color &c, bool includeAlpha = true) {
    if (includeAlpha) {
        return toml::array{c.r, c.g, c.b, c.a};
    }
    return toml::array{c.r, c.g, c.b};
}

// ── Predicate → toml::table ─────────────────────────────────────────────────

toml::table predicateToTable(const PredicateExpr &expr);

toml::table predicateToTable(const PredicateExpr &expr) {
    return std::visit(
        overloaded{
            [](const NameGlobPredicate &p) -> toml::table {
                return toml::table{{"type", "name_glob"}, {"pattern", p.pattern}};
            },
            [](const PathGlobPredicate &p) -> toml::table {
                return toml::table{{"type", "path_glob"}, {"pattern", p.pattern}};
            },
            [](const MaterialGlobPredicate &p) -> toml::table {
                return toml::table{{"type", "material_glob"}, {"pattern", p.pattern}};
            },
            [](const TagPredicate &p) -> toml::table {
                toml::table tbl{{"type", "tag"}, {"key", p.key}};
                if (p.value) {
                    tbl.insert("value", *p.value);
                }
                return tbl;
            },
            [](const IsLeafPredicate &) -> toml::table { return toml::table{{"type", "is_leaf"}}; },
            [](const BoolPredicate &p) -> toml::table {
                // A constant predicate round-trips through the dedicated `bool`
                // type. The older encodings — name_glob "*" for true, empty
                // `and` for false — were lossy: an empty `and` reloads as
                // vacuously true (makeAndPredicate({}) == true), silently
                // inverting an always-false rule into "matches everything".
                return toml::table{{"type", "bool"}, {"value", p.value}};
            },
            [](const std::shared_ptr<AndPredicate> &p) -> toml::table {
                toml::array operands;
                for (const auto &op : p->operands) {
                    operands.push_back(predicateToTable(op));
                }
                return toml::table{{"type", "and"}, {"operands", std::move(operands)}};
            },
            [](const std::shared_ptr<OrPredicate> &p) -> toml::table {
                toml::array operands;
                for (const auto &op : p->operands) {
                    operands.push_back(predicateToTable(op));
                }
                return toml::table{{"type", "or"}, {"operands", std::move(operands)}};
            },
            [](const std::shared_ptr<NotPredicate> &p) -> toml::table {
                return toml::table{{"type", "not"}, {"operand", predicateToTable(p->operand)}};
            },
        },
        expr.data);
}

// ── ExtrasMap → toml::node ──────────────────────────────────────────────────

toml::table extrasToTomlTable(const ExtrasMap &v);
toml::array extrasToTomlArray(const ExtrasMap &v);

/// Hand one extras value to `emit` as the toml type that fits it.
///
/// Null is dropped rather than emitted: TOML has no null, and the if-chain this
/// replaced had no branch for it either.
template <typename Emit> void emitExtras(const ExtrasMap &v, Emit &&emit) {
    std::visit(overloaded{
                   [&](std::monostate) {},
                   [&](bool b) { emit(b); },
                   [&](std::int64_t i) { emit(i); },
                   [&](double d) { emit(d); },
                   [&](const std::string &str) { emit(str); },
                   [&](const ExtrasMap::Array &) { emit(extrasToTomlArray(v)); },
                   [&](const ExtrasMap::Object &) { emit(extrasToTomlTable(v)); },
               },
               v.value());
}

toml::table extrasToTomlTable(const ExtrasMap &v) {
    toml::table tbl;
    if (const auto *obj = v.asObject()) {
        for (const auto &[key, val] : *obj) {
            emitExtras(val, [&](auto &&x) { tbl.insert(key, std::forward<decltype(x)>(x)); });
        }
    }
    return tbl;
}

toml::array extrasToTomlArray(const ExtrasMap &v) {
    toml::array arr;
    if (const auto *elems = v.asArray()) {
        for (const auto &elem : *elems) {
            emitExtras(elem, [&](auto &&x) { arr.push_back(std::forward<decltype(x)>(x)); });
        }
    }
    return arr;
}

// ── Section builders ────────────────────────────────────────────────────────

toml::table buildExportTable(const std::map<std::string, ExportFormatConfig> &formats) {
    toml::table exportTbl;
    for (const auto &[name, variant] : formats) {
        toml::table fmtTbl;
        const auto &common = commonConfig(variant);
        if (common.unitScale) {
            fmtTbl.insert("unit_scale", *common.unitScale);
        }
        std::visit(overloaded{
                       [&](const GltfExportFormatConfig &cfg) {
                           if (cfg.bakeUnitScale) {
                               fmtTbl.insert("bake_unit_scale", *cfg.bakeUnitScale);
                           }
                           if (cfg.multiScene) {
                               fmtTbl.insert("multi_scene", *cfg.multiScene);
                           }
                           if (cfg.sceneNameSeparator) {
                               fmtTbl.insert("scene_name_separator", *cfg.sceneNameSeparator);
                           }
                       },
                       [](const ObjExportFormatConfig &) {},
                   },
                   variant);
        exportTbl.insert(name, std::move(fmtTbl));
    }
    return exportTbl;
}

toml::table buildMaterialsTable(const std::vector<MaterialDef> &materials) {
    using enum AlphaMode;
    toml::table matsTbl;
    for (const auto &mat : materials) {
        toml::table tbl;
        tbl.insert("base_color", colorToArray(mat.baseColor));
        tbl.insert("metallic", mat.metallic);
        tbl.insert("roughness", mat.roughness);
        if (mat.emissive.r != 0.0f || mat.emissive.g != 0.0f || mat.emissive.b != 0.0f) {
            tbl.insert("emissive", colorToArray(mat.emissive, false));
        }
        if (mat.doubleSided) {
            tbl.insert("double_sided", true);
        }
        if (mat.alphaMode != Opaque) {
            tbl.insert("alpha_mode", std::string{alphaModeToString(mat.alphaMode)});
        }
        if (mat.alphaMode == Mask && std::abs(mat.alphaCutoff - 0.5f) > 1e-6f) {
            tbl.insert("alpha_cutoff", mat.alphaCutoff);
        }
        if (mat.ior) {
            tbl.insert("ior", *mat.ior);
        }
        if (mat.transmission) {
            tbl.insert("transmission", *mat.transmission);
        }
        if (mat.clearcoat) {
            tbl.insert("clearcoat", *mat.clearcoat);
        }
        if (mat.clearcoatRoughness) {
            tbl.insert("clearcoat_roughness", *mat.clearcoatRoughness);
        }
        if (mat.anisotropy) {
            tbl.insert("anisotropy", *mat.anisotropy);
        }
        if (mat.anisotropyRotation) {
            tbl.insert("anisotropy_rotation", *mat.anisotropyRotation);
        }
        if (mat.specularFactor) {
            tbl.insert("specular", *mat.specularFactor);
        }
        if (mat.specularColor) {
            tbl.insert("specular_color", colorToArray(*mat.specularColor, false));
        }
        matsTbl.insert(mat.name, std::move(tbl));
    }
    return matsTbl;
}

toml::array buildSelectionRulesArray(const std::vector<SelectionRule> &rules) {
    using enum SelectionAction;
    toml::array arr;
    for (const auto &rule : rules) {
        toml::table tbl;
        if (rule.scope) {
            tbl.insert("scope", *rule.scope);
        }
        const auto actionKey = rule.action == KeepIf ? "keep_if" : "drop_if";
        tbl.insert(actionKey, predicateToTable(rule.predicate));
        arr.push_back(std::move(tbl));
    }
    return arr;
}

toml::array buildRulesArray(const std::vector<Rule> &rules) {
    toml::array arr;
    for (const auto &rule : rules) {
        toml::table tbl;
        if (rule.material) {
            tbl.insert("material", *rule.material);
        }
        if (rule.match) {
            tbl.insert("match", predicateToTable(*rule.match));
        }
        if (rule.tessellation) {
            toml::table tessTbl;
            const auto &tess = *rule.tessellation;
            if (tess.skipGeometry) {
                tessTbl.insert("skip_geometry", *tess.skipGeometry);
            }
            if (tess.mergeDescendants) {
                tessTbl.insert("merge_descendants", *tess.mergeDescendants);
            }
            if (tess.dropCoincidentFaces) {
                tessTbl.insert("drop_coincident_faces", *tess.dropCoincidentFaces);
            }
            if (tess.averageMaterialStack) {
                tessTbl.insert("average_material_stack", *tess.averageMaterialStack);
            }
            if (tess.maxSegmentsCircle) {
                tessTbl.insert("max_segments_circle",
                               static_cast<int64_t>(*tess.maxSegmentsCircle));
            }
            if (tess.fallback) {
                tessTbl.insert("fallback", std::string{booleanFallbackToString(*tess.fallback)});
            }
            tbl.insert("tessellation", std::move(tessTbl));
        }
        if (rule.extras) {
            tbl.insert("extras", extrasToTomlTable(*rule.extras));
        }
        arr.push_back(std::move(tbl));
    }
    return arr;
}

// Build the `[defaults]` sub-tree: cfg.tessellationDefaults + cfg.extrasDefaults.
// Returns an empty table when nothing is set, in which case configToToml omits
// the key entirely. Mirrors the parser at config_loader.cpp:746-771 — those
// are the only two keys recognised under [defaults].
toml::table buildDefaultsTable(const NHConfig &cfg) {
    toml::table out;
    const auto &td = cfg.tessellationDefaults;
    const bool any_tess = td.skipGeometry || td.mergeDescendants || td.dropCoincidentFaces ||
                          td.averageMaterialStack || td.maxSegmentsCircle || td.fallback;
    if (any_tess) {
        toml::table tessTbl;
        if (td.skipGeometry) {
            tessTbl.insert("skip_geometry", *td.skipGeometry);
        }
        if (td.mergeDescendants) {
            tessTbl.insert("merge_descendants", *td.mergeDescendants);
        }
        if (td.dropCoincidentFaces) {
            tessTbl.insert("drop_coincident_faces", *td.dropCoincidentFaces);
        }
        if (td.averageMaterialStack) {
            tessTbl.insert("average_material_stack", *td.averageMaterialStack);
        }
        if (td.maxSegmentsCircle) {
            tessTbl.insert("max_segments_circle", static_cast<int64_t>(*td.maxSegmentsCircle));
        }
        if (td.fallback) {
            tessTbl.insert("fallback", std::string{booleanFallbackToString(*td.fallback)});
        }
        out.insert("tessellation", std::move(tessTbl));
    }
    if (cfg.extrasDefaults) {
        out.insert("extras", extrasToTomlTable(*cfg.extrasDefaults));
    }
    return out;
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────────

std::string configToToml(const NHConfig &cfg) {
    toml::table root;

    if (cfg.hoistOrphans) {
        root.insert("hoist_orphans", true);
    }
    if (!cfg.deduplicateShapes) {
        root.insert("deduplicate_shapes", false);
    }
    if (!cfg.exportFormats.empty()) {
        root.insert("export", buildExportTable(cfg.exportFormats));
    }
    if (!cfg.materials.empty()) {
        root.insert("materials", buildMaterialsTable(cfg.materials));
    }
    if (!cfg.selection.empty()) {
        root.insert("selection_rules", buildSelectionRulesArray(cfg.selection));
    }
    if (!cfg.rules.empty()) {
        root.insert("rules", buildRulesArray(cfg.rules));
    }
    if (auto defaults = buildDefaultsTable(cfg); !defaults.empty()) {
        root.insert("defaults", std::move(defaults));
    }

    std::ostringstream os;
    os << root;
    return os.str();
}

} // namespace nodehammer::config
