#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <toml++/toml.hpp>

#include <format>
#include <utility>

namespace nodehammer {

namespace {

// ── Unknown-key helper ────────────────────────────────────────────────────────

template <typename Range>
void warnUnknownKeysImpl(const toml::table &tbl, const Range &known, std::string_view context,
                         DiagnosticList &diags) {
    for (const auto &[key, _] : tbl) {
        bool found = false;
        for (auto k : known) {
            if (key.str() == k) {
                found = true;
                break;
            }
        }
        if (!found) {
            diags.warn(codes::kWarnConfigUnknownKey, std::format("unknown key '{}'", key.str()),
                       context);
        }
    }
}

void warnUnknownKeys(const toml::table &tbl, std::initializer_list<std::string_view> known,
                     std::string_view context, DiagnosticList &diags) {
    warnUnknownKeysImpl(tbl, known, context, diags);
}

void warnUnknownKeys(const toml::table &tbl, const std::vector<std::string_view> &known,
                     std::string_view context, DiagnosticList &diags) {
    warnUnknownKeysImpl(tbl, known, context, diags);
}

// ── Enum parsers ──────────────────────────────────────────────────────────────

std::optional<ClosurePolicy> parseClosure(std::string_view s) {
    if (s == "none") {
        return ClosurePolicy::None;
    }
    if (s == "ancestors") {
        return ClosurePolicy::Ancestors;
    }
    if (s == "descendants") {
        return ClosurePolicy::Descendants;
    }
    if (s == "full") {
        return ClosurePolicy::Full;
    }
    return std::nullopt;
}

std::optional<BooleanFallback> parseFallback(std::string_view s) {
    if (s == "skip") {
        return BooleanFallback::Skip;
    }
    if (s == "bbox") {
        return BooleanFallback::BBox;
    }
    if (s == "fail") {
        return BooleanFallback::Fail;
    }
    return std::nullopt;
}

// ── Predicate parser (recursive) ─────────────────────────────────────────────

std::optional<PredicateExpr> parsePredicate(const toml::table &tbl, DiagnosticList &diags,
                                            std::string_view context);

std::optional<PredicateExpr> parsePredicate(const toml::table &tbl, DiagnosticList &diags,
                                            std::string_view context) {
    auto typeOpt = tbl["type"].value<std::string>();
    if (!typeOpt) {
        diags.error(codes::kErrConfigParse, "predicate is missing required 'type' field", context);
        return std::nullopt;
    }
    const std::string &type = *typeOpt;

    if (type == "name_glob") {
        auto pat = tbl["pattern"].value<std::string>();
        if (!pat) {
            diags.error(codes::kErrConfigParse, "name_glob predicate missing 'pattern'", context);
            return std::nullopt;
        }
        return PredicateExpr{NameGlobPredicate{*pat}};
    }

    if (type == "path_glob") {
        auto pat = tbl["pattern"].value<std::string>();
        if (!pat) {
            diags.error(codes::kErrConfigParse, "path_glob predicate missing 'pattern'", context);
            return std::nullopt;
        }
        return PredicateExpr{PathGlobPredicate{*pat}};
    }

    if (type == "tag") {
        auto key = tbl["key"].value<std::string>();
        if (!key) {
            diags.error(codes::kErrConfigParse, "tag predicate missing 'key'", context);
            return std::nullopt;
        }
        return PredicateExpr{TagPredicate{*key, tbl["value"].value<std::string>()}};
    }

    if (type == "is_leaf") {
        return PredicateExpr{IsLeafPredicate{}};
    }

    if (type == "and" || type == "or") {
        const auto *operandsArr = tbl["operands"].as_array();
        if (operandsArr == nullptr) {
            diags.error(codes::kErrConfigParse,
                        std::format("'{}' predicate missing 'operands' array", type), context);
            return std::nullopt;
        }
        std::vector<PredicateExpr> operands;
        operands.reserve(operandsArr->size());
        for (const auto &elem : *operandsArr) {
            const auto *subTbl = elem.as_table();
            if (subTbl == nullptr) {
                diags.error(codes::kErrConfigParse, "operand in compound predicate must be a table",
                            context);
                return std::nullopt;
            }
            auto sub = parsePredicate(*subTbl, diags, context);
            if (!sub) {
                return std::nullopt;
            }
            operands.push_back(std::move(*sub));
        }
        if (type == "and") {
            return PredicateExpr{std::make_shared<AndPredicate>(std::move(operands))};
        }
        return PredicateExpr{std::make_shared<OrPredicate>(std::move(operands))};
    }

    if (type == "not") {
        const auto *operandTbl = tbl["operand"].as_table();
        if (operandTbl == nullptr) {
            diags.error(codes::kErrConfigParse, "'not' predicate missing 'operand' table", context);
            return std::nullopt;
        }
        auto sub = parsePredicate(*operandTbl, diags, context);
        if (!sub) {
            return std::nullopt;
        }
        return PredicateExpr{std::make_shared<NotPredicate>(std::move(*sub))};
    }

    diags.error(codes::kErrConfigParse, std::format("unknown predicate type '{}'", type), context);
    return std::nullopt;
}

// ── Section parsers ───────────────────────────────────────────────────────────

/// [materials.<name>] — the table key is the material name.
void parseMaterials(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *matsTable = root["materials"].as_table();
    if (matsTable == nullptr) {
        return;
    }
    for (const auto &[key, node] : *matsTable) {
        const auto *tbl = node.as_table();
        if (tbl == nullptr) {
            diags.error(codes::kErrConfigParse,
                        std::format("materials.{} must be a table, not a scalar", key.str()));
            continue;
        }
        warnUnknownKeys(*tbl, {"base_color", "metallic", "roughness", "double_sided", "emissive"},
                        std::format("materials.{}", key.str()), diags);
        MaterialDef def;
        def.name = key.str();

        if (const auto *colorArr = (*tbl)["base_color"].as_array()) {
            if (colorArr->size() >= 4) {
                def.baseColor.r = colorArr->at(0).value<float>().value_or(def.baseColor.r);
                def.baseColor.g = colorArr->at(1).value<float>().value_or(def.baseColor.g);
                def.baseColor.b = colorArr->at(2).value<float>().value_or(def.baseColor.b);
                def.baseColor.a = colorArr->at(3).value<float>().value_or(def.baseColor.a);
            }
        }
        def.metallic = (*tbl)["metallic"].value<float>().value_or(def.metallic);
        def.roughness = (*tbl)["roughness"].value<float>().value_or(def.roughness);
        def.doubleSided = (*tbl)["double_sided"].value<bool>().value_or(def.doubleSided);
        if (const auto *emissArr = (*tbl)["emissive"].as_array()) {
            if (emissArr->size() >= 3) {
                def.emissive.r = emissArr->at(0).value<float>().value_or(def.emissive.r);
                def.emissive.g = emissArr->at(1).value<float>().value_or(def.emissive.g);
                def.emissive.b = emissArr->at(2).value<float>().value_or(def.emissive.b);
            }
        }
        cfg.materials.push_back(std::move(def));
    }
}

/// [[selection_rules]] — action determined by presence of keep_if or drop_if key.
void parseSelectionRules(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["selection_rules"].as_array();
    if (arr == nullptr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (tbl == nullptr) {
            continue;
        }

        warnUnknownKeys(*tbl, {"keep_if", "drop_if", "scope", "closure"}, "selection_rules", diags);

        SelectionAction action{};
        const toml::table *predTbl = nullptr;

        if (const auto *kif = (*tbl)["keep_if"].as_table()) {
            action = SelectionAction::KeepIf;
            predTbl = kif;
        } else if (const auto *dif = (*tbl)["drop_if"].as_table()) {
            action = SelectionAction::DropIf;
            predTbl = dif;
        } else {
            diags.error(codes::kErrConfigParse,
                        "selection_rule must have either a 'keep_if' or 'drop_if' table");
            continue;
        }

        auto pred = parsePredicate(*predTbl, diags, "selection_rules");
        if (!pred) {
            continue;
        }

        ClosurePolicy closure = ClosurePolicy::None;
        if (auto closureStr = (*tbl)["closure"].value<std::string>()) {
            auto parsed = parseClosure(*closureStr);
            if (!parsed) {
                diags.error(codes::kErrConfigParse,
                            std::format("unknown closure '{}'; expected none, ancestors, "
                                        "descendants, or full",
                                        *closureStr));
                continue;
            }
            closure = *parsed;
        }

        SelectionRule rule;
        rule.action = action;
        rule.predicate = std::move(*pred);
        rule.closure = closure;
        if (auto scope = (*tbl)["scope"].value<std::string>()) {
            rule.scope = std::move(*scope);
        }
        cfg.selection.push_back(std::move(rule));
    }
}

/// [[material_rules]] — scope (optional), match (optional predicate), material (required).
/// Future: match may also be a string expression DSL (e.g. match = "tag.semantic == sensor").
void parseMaterialRules(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["material_rules"].as_array();
    if (arr == nullptr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (tbl == nullptr) {
            continue;
        }

        warnUnknownKeys(*tbl, {"scope", "match", "material"}, "material_rules", diags);
        auto material = (*tbl)["material"].value<std::string>();
        if (!material) {
            diags.error(codes::kErrConfigParse, "material_rule missing required 'material' field");
            continue;
        }

        MaterialRule rule;
        rule.materialName = *material;

        if (auto scope = (*tbl)["scope"].value<std::string>()) {
            rule.scope = std::move(*scope);
        }
        if (const auto *matchTbl = (*tbl)["match"].as_table()) {
            auto pred = parsePredicate(*matchTbl, diags, "material_rules");
            if (!pred) {
                continue;
            }
            rule.match = std::move(*pred);
        } else if ((*tbl)["match"].as_string() != nullptr) {
            diags.error(codes::kErrConfigParse,
                        "string expression DSL for 'match' is not yet supported; use a table",
                        "material_rules");
            continue;
        }
        cfg.materialRules.push_back(std::move(rule));
    }
}

/// [[tessellation_rules]] — scope (optional), max_segments_circle, fallback.
void parseTessellationRules(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["tessellation_rules"].as_array();
    if (arr == nullptr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (tbl == nullptr) {
            continue;
        }
        warnUnknownKeys(
            *tbl, {"scope", "skip_geometry", "merge_children", "max_segments_circle", "fallback"},
            "tessellation_rules", diags);
        TessellationRule rule;
        rule.skipGeometry = (*tbl)["skip_geometry"].value<bool>().value_or(false);
        rule.mergeChildren = (*tbl)["merge_children"].value<bool>().value_or(false);
        rule.maxSegmentsCircle = (*tbl)["max_segments_circle"].value<int>().value_or(64);

        if (auto fallbackStr = (*tbl)["fallback"].value<std::string>()) {
            auto parsed = parseFallback(*fallbackStr);
            if (!parsed) {
                diags.error(codes::kErrConfigParse,
                            std::format("unknown fallback '{}'; expected skip, bbox, or fail",
                                        *fallbackStr),
                            "tessellation_rules");
                continue;
            }
            rule.fallback = *parsed;
        }

        // scope can be a string or an array of strings; flatten into one rule per scope.
        if (const auto *scopeArr = (*tbl)["scope"].as_array()) {
            for (const auto &scopeEntry : *scopeArr) {
                if (auto s = scopeEntry.value<std::string>()) {
                    TessellationRule copy = rule;
                    copy.scope = std::move(*s);
                    cfg.tessellationRules.push_back(std::move(copy));
                }
            }
        } else if (auto scope = (*tbl)["scope"].value<std::string>()) {
            rule.scope = std::move(*scope);
            cfg.tessellationRules.push_back(std::move(rule));
        } else {
            cfg.tessellationRules.push_back(std::move(rule));
        }
    }
}

// ── Top-level parse driver ────────────────────────────────────────────────────

ConfigResult parseTable(const toml::table &tbl) {
    DiagnosticList diags;
    NHConfig cfg;

    warnUnknownKeys(tbl,
                    {"hoist_orphans", "export", "materials", "selection_rules", "material_rules",
                     "tessellation_rules"},
                    "<top-level>", diags);
    if (auto v = tbl["hoist_orphans"].value<bool>()) {
        cfg.hoistOrphans = *v;
    }
    if (const auto *exportTbl = tbl["export"].as_table()) {
        // ── Table-driven export config ───────────────────────────────────────
        // Each key declares which formats accept it (empty = all).
        // Each format group defines a factory that constructs its variant type
        // from the parsed TOML table + common config.
        struct ExportKeyDef {
            std::string_view key;
            std::initializer_list<std::string_view> formats;
        };
        static constexpr ExportKeyDef kExportKeys[] = {
            {"unit_scale", {}},
            {"bake_unit_scale", {}},
            {"multi_scene", {"gltf", "glb"}},
            {"scene_name_separator", {"gltf", "glb"}},
        };

        struct FormatGroup {
            std::initializer_list<std::string_view> formats;
            ExportFormatConfig (*build)(const toml::table &, const CommonExportConfig &);
        };
        static const FormatGroup kFormatGroups[] = {
            {{"gltf", "glb"},
             [](const toml::table &t, const CommonExportConfig &c) -> ExportFormatConfig {
                 GltfExportFormatConfig cfg;
                 cfg.common = c;
                 if (auto v = t["multi_scene"].value<bool>()) {
                     cfg.multiScene = *v;
                 }
                 if (auto v = t["scene_name_separator"].value<std::string>()) {
                     cfg.sceneNameSeparator = std::move(*v);
                 }
                 return cfg;
             }},
            {{"obj"},
             [](const toml::table &, const CommonExportConfig &c) -> ExportFormatConfig {
                 ObjExportFormatConfig cfg;
                 cfg.common = c;
                 return cfg;
             }},
        };

        for (const auto &[fmtKeyRaw, fmtVal] : *exportTbl) {
            const auto *fmtTbl = fmtVal.as_table();
            if (fmtTbl == nullptr) {
                continue;
            }
            const std::string fmtKey{fmtKeyRaw.str()};
            const std::string ctx = std::format("export.{}", fmtKey);

            // Build the valid key set for this format and check for misplaced keys.
            std::vector<std::string_view> validKeys;
            for (const auto &kd : kExportKeys) {
                const bool allowed =
                    kd.formats.size() == 0 ||
                    std::find(kd.formats.begin(), kd.formats.end(), fmtKey) != kd.formats.end();
                if (allowed) {
                    validKeys.push_back(kd.key);
                } else if ((*fmtTbl)[kd.key]) {
                    std::string validFmts;
                    for (auto it = kd.formats.begin(); it != kd.formats.end(); ++it) {
                        if (!validFmts.empty()) {
                            validFmts += " or ";
                        }
                        validFmts += std::format("[export.{}]", *it);
                    }
                    diags.error(codes::kErrConfigParse,
                                std::format("'{}' is only valid under {}, not [export.{}]", kd.key,
                                            validFmts, fmtKey),
                                ctx);
                }
            }
            warnUnknownKeys(*fmtTbl, validKeys, ctx, diags);

            // Parse common values.
            CommonExportConfig common;
            if (auto v = (*fmtTbl)["unit_scale"].value<double>()) {
                common.unitScale = *v;
            }
            if (auto v = (*fmtTbl)["bake_unit_scale"].value<bool>()) {
                common.bakeUnitScale = *v;
            }

            // Find the matching format group and build the variant.
            bool matched = false;
            for (const auto &fg : kFormatGroups) {
                if (std::find(fg.formats.begin(), fg.formats.end(), fmtKey) != fg.formats.end()) {
                    cfg.exportFormats[fmtKey] = fg.build(*fmtTbl, common);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                diags.warn(codes::kWarnConfigUnknownKey,
                           std::format("unknown export format '{}'", fmtKey), ctx);
            }
        }
    }
    parseMaterials(tbl, cfg, diags);
    parseSelectionRules(tbl, cfg, diags);
    parseMaterialRules(tbl, cfg, diags);
    parseTessellationRules(tbl, cfg, diags);

    return ConfigResult{std::move(cfg), std::move(diags)};
}

} // namespace

// ── ConfigLoader ──────────────────────────────────────────────────────────────

ConfigResult ConfigLoader::loadFromFile(const std::filesystem::path &path) {
    DiagnosticList diags;
    try {
        auto tbl = toml::parse_file(path.string());
        return parseTable(tbl);
    } catch (const toml::parse_error &e) {
        diags.error(
            codes::kErrConfigParse, e.description(),
            std::format("{}:{}:{}", path.string(), e.source().begin.line, e.source().begin.column));
    } catch (const std::exception &e) {
        diags.error(codes::kErrImportFileNotFound,
                    std::format("could not read config file: {}", e.what()), path.string());
    }
    return ConfigResult{{}, std::move(diags)};
}

ConfigResult ConfigLoader::loadFromString(std::string_view content, std::string_view sourceName) {
    DiagnosticList diags;
    try {
        auto tbl = toml::parse(content, sourceName);
        return parseTable(tbl);
    } catch (const toml::parse_error &e) {
        diags.error(
            codes::kErrConfigParse, e.description(),
            std::format("{}:{}:{}", sourceName, e.source().begin.line, e.source().begin.column));
    }
    return ConfigResult{{}, std::move(diags)};
}

} // namespace nodehammer
