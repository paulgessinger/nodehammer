#include <nodehammer/config/config_enums.hpp>
#include <nodehammer/config/config_writer.hpp>
#include <nodehammer/detail/overloaded.hpp>

#include <toml++/toml.hpp>

#include <sstream>

namespace nodehammer {

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

// ── JSON → toml::node ───────────────────────────────────────────────────────

toml::table jsonToTomlTable(const nlohmann::json &j);
toml::array jsonToTomlArray(const nlohmann::json &j);

toml::table jsonToTomlTable(const nlohmann::json &j) {
    toml::table tbl;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_object()) {
            tbl.insert(it.key(), jsonToTomlTable(it.value()));
        } else if (it.value().is_array()) {
            tbl.insert(it.key(), jsonToTomlArray(it.value()));
        } else if (it.value().is_string()) {
            tbl.insert(it.key(), it.value().get<std::string>());
        } else if (it.value().is_boolean()) {
            tbl.insert(it.key(), it.value().get<bool>());
        } else if (it.value().is_number_integer()) {
            tbl.insert(it.key(), it.value().get<int64_t>());
        } else if (it.value().is_number_float()) {
            tbl.insert(it.key(), it.value().get<double>());
        }
    }
    return tbl;
}

toml::array jsonToTomlArray(const nlohmann::json &j) {
    toml::array arr;
    for (const auto &elem : j) {
        if (elem.is_object()) {
            arr.push_back(jsonToTomlTable(elem));
        } else if (elem.is_array()) {
            arr.push_back(jsonToTomlArray(elem));
        } else if (elem.is_string()) {
            arr.push_back(elem.get<std::string>());
        } else if (elem.is_boolean()) {
            arr.push_back(elem.get<bool>());
        } else if (elem.is_number_integer()) {
            arr.push_back(elem.get<int64_t>());
        } else if (elem.is_number_float()) {
            arr.push_back(elem.get<double>());
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
        if (common.bakeUnitScale) {
            fmtTbl.insert("bake_unit_scale", *common.bakeUnitScale);
        }
        std::visit(overloaded{
                       [&](const GltfExportFormatConfig &cfg) {
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
            if (tess.mergeCoincident) {
                tessTbl.insert("merge_coincident", *tess.mergeCoincident);
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
            tbl.insert("extras", jsonToTomlTable(*rule.extras));
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
    const bool any_tess = td.skipGeometry || td.mergeDescendants || td.mergeCoincident ||
                          td.maxSegmentsCircle || td.fallback;
    if (any_tess) {
        toml::table tessTbl;
        if (td.skipGeometry) {
            tessTbl.insert("skip_geometry", *td.skipGeometry);
        }
        if (td.mergeDescendants) {
            tessTbl.insert("merge_descendants", *td.mergeDescendants);
        }
        if (td.mergeCoincident) {
            tessTbl.insert("merge_coincident", *td.mergeCoincident);
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
        out.insert("extras", jsonToTomlTable(*cfg.extrasDefaults));
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

} // namespace nodehammer
