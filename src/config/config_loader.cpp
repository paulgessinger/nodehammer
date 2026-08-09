#include <config/color_parse.hpp>
#include <config/config_enums.hpp>
#include <config/config_keys.hpp>
#include <config/config_loader.hpp>
#include <config/predicate_parser.hpp>
#include <detail/file_io.hpp>
#include <diagnostic_codes.hpp>

#include <toml++/toml.hpp>

#include <format>
#include <memory>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>

namespace nodehammer::config {

namespace {

// ── Include resolution ───────────────────────────────────────────────────────
//
// Recursively processes `include = "file"` or `include = ["file1", "file2"]`.
// Included files are parsed, their own includes resolved, and then merged
// into the parent table. Arrays are concatenated, tables are deep-merged,
// scalars use last-wins (parent overrides included values).

/// Deep-merge `src` into `dst`.
///   - Tables are merged recursively.
///   - Arrays are concatenated: dst entries first, then src entries appended.
///   - Scalars: dst wins if already present (first-wins).
void mergeToml(toml::table &dst, const toml::table &src) {
    for (const auto &[key, val] : src) {
        const std::string k{key.str()};
        if (k == "include") {
            continue;
        }

        if (!dst.contains(k)) {
            dst.insert(k, val);
            continue;
        }

        auto &existing = *dst.get(k);

        if (existing.is_table() && val.is_table()) {
            mergeToml(*existing.as_table(), *val.as_table());
            continue;
        }

        if (existing.is_array() && val.is_array()) {
            for (const auto &e : *val.as_array()) {
                existing.as_array()->push_back(e);
            }
            continue;
        }
    }
}

/// Byte-driven include resolver. Both sets track string keys instead of
/// canonical filesystem paths — the caller's key scheme decides equivalence
/// (URL keys are pre-normalised, bag keys are basenames, etc.). Each include is
/// computed via the same `parent_dir / rel` arithmetic the filesystem variant
/// uses, then resolved through the supplied `fetcher`.
///
/// The two sets answer different questions, and conflating them is what made a
/// diamond look like a cycle:
///
///   - `merged` is every key pulled in anywhere in the tree. A second
///     encounter means the content is already present, so it is skipped
///     silently — `top -> {b, c} -> base` merges base once, which is also what
///     keeps `[[rules]]` and `[[selection_rules]]` from being duplicated by a
///     shared base.
///   - `onPath` is only the chain currently being resolved, and is therefore
///     passed **by value** so sibling branches never see each other's. A hit
///     here means an include reaches one of its own ancestors: a real cycle,
///     which would not terminate, so it is an error.
void resolveIncludesFromBytes(toml::table &tbl, std::string_view parent_key,
                              const IncludeFetcher &fetcher, std::set<std::string> &merged,
                              std::set<std::string> onPath, diagnostics::List &diags) {
    const auto *includeNode = tbl.get("include");
    if (includeNode == nullptr) {
        return;
    }

    std::vector<std::string> paths;
    if (includeNode->is_string()) {
        paths.push_back(std::string{includeNode->as_string()->get()});
    } else if (includeNode->is_array()) {
        for (const auto &e : *includeNode->as_array()) {
            if (auto s = e.value<std::string>()) {
                paths.push_back(std::move(*s));
            }
        }
    } else {
        diags.warn(codes::kWarnConfigUnknownKey, "'include' must be a string or array of strings",
                   "<include>");
        return;
    }

    tbl.erase("include");

    std::vector<toml::table> includedTables;
    for (const auto &relPath : paths) {
        std::string absKey = ConfigLoader::resolveIncludeKey(parent_key, relPath);

        if (onPath.contains(absKey)) {
            diags.error(codes::kErrConfigParse,
                        std::format("circular include detected: '{}'", absKey), "<include>");
            continue;
        }
        if (!merged.insert(absKey).second) {
            continue; // already merged via another branch; not a cycle
        }

        auto bytes = fetcher(absKey);
        if (!bytes) {
            diags.error(codes::kFatalImportFileNotFound,
                        std::format("include not found: '{}'", absKey), "<include>");
            continue;
        }

        toml::table included;
        try {
            included = toml::parse(
                std::string_view{reinterpret_cast<const char *>(bytes->data()), bytes->size()},
                absKey);
        } catch (const toml::parse_error &e) {
            diags.error(
                codes::kErrConfigParse, e.description(),
                std::format("{}:{}:{}", absKey, e.source().begin.line, e.source().begin.column));
            continue;
        }

        auto childPath = onPath;
        childPath.insert(absKey);
        resolveIncludesFromBytes(included, absKey, fetcher, merged, std::move(childPath), diags);
        includedTables.push_back(std::move(included));
    }

    toml::table combined;
    for (auto &inc : includedTables) {
        mergeToml(combined, inc);
    }
    mergeToml(combined, tbl);
    tbl = std::move(combined);
}

// ── Unknown-key helper ────────────────────────────────────────────────────────

template <typename Range>
void warnUnknownKeysImpl(const toml::table &tbl, const Range &known, std::string_view context,
                         diagnostics::List &diags) {
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

// Overload for the shared, fixed allowlists in config_keys.hpp (std::array
// converts to this span). This is the path the section parsers use so their
// valid-key set stays identical to the writer's and the Lua front-end's.
void warnUnknownKeys(const toml::table &tbl, std::span<const std::string_view> known,
                     std::string_view context, diagnostics::List &diags) {
    warnUnknownKeysImpl(tbl, known, context, diags);
}

// Overload for the export section, whose valid keys depend on the format and are
// assembled into a vector at parse time.
void warnUnknownKeys(const toml::table &tbl, const std::vector<std::string_view> &known,
                     std::string_view context, diagnostics::List &diags) {
    warnUnknownKeysImpl(tbl, known, context, diags);
}

// ── Predicate parser (recursive) ─────────────────────────────────────────────

std::optional<PredicateExpr> parsePredicate(const toml::table &tbl, diagnostics::List &diags,
                                            std::string_view context);

std::optional<PredicateExpr> parsePredicate(const toml::table &tbl, diagnostics::List &diags,
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

    if (type == "material_glob") {
        auto pat = tbl["pattern"].value<std::string>();
        if (!pat) {
            diags.error(codes::kErrConfigParse, "material_glob predicate missing 'pattern'",
                        context);
            return std::nullopt;
        }
        return PredicateExpr{MaterialGlobPredicate{*pat}};
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

    if (type == "bool") {
        auto val = tbl["value"].value<bool>();
        if (!val) {
            diags.error(codes::kErrConfigParse, "bool predicate missing 'value'", context);
            return std::nullopt;
        }
        return PredicateExpr{BoolPredicate{*val}};
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
void parseMaterials(const toml::table &root, NHConfig &cfg, diagnostics::List &diags) {
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
        warnUnknownKeys(*tbl, keys::kMaterialKeys, std::format("materials.{}", key.str()), diags);
        MaterialDef def;
        def.name = key.str();

        if (const auto *colorArr = (*tbl)["base_color"].as_array()) {
            if (colorArr->size() >= 3) {
                def.baseColor.r = colorArr->at(0).value<float>().value_or(def.baseColor.r);
                def.baseColor.g = colorArr->at(1).value<float>().value_or(def.baseColor.g);
                def.baseColor.b = colorArr->at(2).value<float>().value_or(def.baseColor.b);
                if (colorArr->size() >= 4) {
                    def.baseColor.a = colorArr->at(3).value<float>().value_or(def.baseColor.a);
                }
            }
        } else if (auto colorStr = (*tbl)["base_color"].value<std::string>()) {
            if (auto parsed = parseHexColor(*colorStr)) {
                def.baseColor = *parsed;
            } else {
                diags.warn(codes::kWarnConfigUnknownKey,
                           std::format("materials.{}: invalid hex color '{}'; "
                                       "expected #RRGGBB or #RRGGBBAA",
                                       key.str(), *colorStr),
                           std::format("materials.{}", key.str()));
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
        if (auto am = (*tbl)["alpha_mode"].value<std::string>()) {
            if (auto parsed = parseAlphaMode(*am)) {
                def.alphaMode = *parsed;
            } else {
                diags.warn(codes::kWarnConfigUnknownKey,
                           std::format("materials.{}: unknown alpha_mode '{}'; "
                                       "expected opaque, mask, or blend",
                                       key.str(), *am),
                           std::format("materials.{}", key.str()));
            }
        }
        def.alphaCutoff = (*tbl)["alpha_cutoff"].value<float>().value_or(def.alphaCutoff);
        if (auto v = (*tbl)["ior"].value<float>()) {
            def.ior = *v;
        }
        if (auto v = (*tbl)["transmission"].value<float>()) {
            def.transmission = *v;
        }
        if (auto v = (*tbl)["clearcoat"].value<float>()) {
            def.clearcoat = *v;
        }
        if (auto v = (*tbl)["clearcoat_roughness"].value<float>()) {
            def.clearcoatRoughness = *v;
        }
        if (auto v = (*tbl)["anisotropy"].value<float>()) {
            def.anisotropy = *v;
        }
        if (auto v = (*tbl)["anisotropy_rotation"].value<float>()) {
            def.anisotropyRotation = *v;
        }
        if (auto v = (*tbl)["specular"].value<float>()) {
            def.specularFactor = *v;
        }
        if (const auto *specColorArr = (*tbl)["specular_color"].as_array()) {
            if (specColorArr->size() >= 3) {
                Color c;
                c.r = specColorArr->at(0).value<float>().value_or(1.0f);
                c.g = specColorArr->at(1).value<float>().value_or(1.0f);
                c.b = specColorArr->at(2).value<float>().value_or(1.0f);
                def.specularColor = c;
            }
        }
        cfg.materials.push_back(std::move(def));
    }
}

/// [[selection_rules]] — action determined by presence of keep_if or drop_if key.
void parseSelectionRules(const toml::table &root, NHConfig &cfg, diagnostics::List &diags) {
    const auto *arr = root["selection_rules"].as_array();
    if (arr == nullptr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (tbl == nullptr) {
            continue;
        }

        warnUnknownKeys(*tbl, keys::kSelectionRuleKeys, "selection_rules", diags);

        SelectionAction action{};
        std::optional<PredicateExpr> pred;

        // keep_if / drop_if can be:
        //   - a table (structured predicate)
        //   - a string (expression)
        //   - an array of strings (each parsed, combined with OR)
        for (const auto &[actKey, actVal] :
             std::initializer_list<std::pair<std::string_view, SelectionAction>>{
                 {"keep_if", SelectionAction::KeepIf}, {"drop_if", SelectionAction::DropIf}}) {
            if (const auto *sub = (*tbl)[actKey].as_table()) {
                action = actVal;
                pred = parsePredicate(*sub, diags, "selection_rules");
                break;
            }
            if (auto str = (*tbl)[actKey].value<std::string>()) {
                action = actVal;
                auto parsed = parsePredicateExpr(*str);
                if (!parsed) {
                    diags.error(
                        codes::kErrConfigParse,
                        std::format("failed to parse '{}' expression: {}", actKey, parsed.error()),
                        "selection_rules");
                }
                pred = std::move(parsed).value_or(PredicateExpr{BoolPredicate{false}});
                break;
            }
            if (const auto *predArr = (*tbl)[actKey].as_array()) {
                action = actVal;
                std::vector<PredicateExpr> operands;
                bool ok = true;
                for (const auto &elem : *predArr) {
                    if (auto s = elem.value<std::string>()) {
                        auto p = parsePredicateExpr(*s);
                        if (!p) {
                            diags.error(codes::kErrConfigParse,
                                        std::format("failed to parse '{}' expression: {}", actKey,
                                                    p.error()),
                                        "selection_rules");
                            ok = false;
                            break;
                        }
                        operands.push_back(std::move(*p));
                    }
                }
                if (!ok) {
                    break;
                }
                if (operands.empty()) {
                    diags.warn(codes::kWarnConfigEmptyScope,
                               std::format("'{}' is an empty array (all entries commented out?) "
                                           "-- rule will be skipped",
                                           actKey),
                               "selection_rules");
                    break;
                }
                pred = combineOr(std::move(operands));
                break;
            }
        }
        if (!pred) {
            diags.error(codes::kErrConfigParse,
                        "selection_rule must have either a 'keep_if' or 'drop_if' field");
            continue;
        }

        SelectionRule rule;
        rule.action = action;
        rule.predicate = std::move(*pred);
        if (auto scope = (*tbl)["scope"].value<std::string>()) {
            rule.scope = std::move(*scope);
        }
        cfg.selection.push_back(std::move(rule));
    }
}

/// Recursively convert a TOML value to nlohmann::json.
nlohmann::json tomlToJson(const toml::node &node) {
    if (node.is_boolean()) {
        return node.as_boolean()->get();
    }
    if (node.is_integer()) {
        return node.as_integer()->get();
    }
    if (node.is_floating_point()) {
        return node.as_floating_point()->get();
    }
    if (node.is_string()) {
        return std::string{node.as_string()->get()};
    }
    if (node.is_array()) {
        auto j = nlohmann::json::array();
        for (const auto &elem : *node.as_array()) {
            j.push_back(tomlToJson(elem));
        }
        return j;
    }
    if (node.is_table()) {
        auto j = nlohmann::json::object();
        for (const auto &[key, val] : *node.as_table()) {
            j[std::string{key.str()}] = tomlToJson(val);
        }
        return j;
    }
    return nullptr;
}

/// Parse an extras sub-table into an ExtrasMap (nlohmann::json object).
ExtrasMap parseExtras(const toml::table &tbl) { return tomlToJson(tbl); }

/// [[rules]] — unified rule with optional material, tessellation, and extras.
void parseRules(const toml::table &root, NHConfig &cfg, diagnostics::List &diags) {
    const auto *arr = root["rules"].as_array();
    if (arr == nullptr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (tbl == nullptr) {
            continue;
        }

        warnUnknownKeys(*tbl, keys::kRuleKeys, "rules", diags);

        Rule rule;

        // ── match predicate (table, string, or array of strings) ────────────
        if (const auto *matchTbl = (*tbl)["match"].as_table()) {
            auto pred = parsePredicate(*matchTbl, diags, "rules");
            if (!pred) {
                continue;
            }
            rule.match = std::move(*pred);
        } else if (auto matchStr = (*tbl)["match"].value<std::string>()) {
            auto parsed = parsePredicateExpr(*matchStr);
            if (!parsed) {
                diags.error(codes::kErrConfigParse,
                            std::format("failed to parse 'match' expression: {}", parsed.error()),
                            "rules");
                continue;
            }
            rule.match = std::move(*parsed);
        } else if (const auto *matchArr = (*tbl)["match"].as_array()) {
            // Array of string expressions → OR'd together.
            if (matchArr->empty()) {
                diags.warn(codes::kWarnConfigEmptyScope,
                           "match is an empty array -- rule will match nothing; "
                           "remove 'match' entirely to match all nodes",
                           "rules");
                continue;
            }
            std::vector<PredicateExpr> operands;
            bool ok = true;
            for (const auto &elem : *matchArr) {
                auto s = elem.value<std::string>();
                if (!s) {
                    continue;
                }
                auto parsed = parsePredicateExpr(*s);
                if (!parsed) {
                    diags.error(
                        codes::kErrConfigParse,
                        std::format("failed to parse 'match' expression: {}", parsed.error()),
                        "rules");
                    ok = false;
                    break;
                }
                operands.push_back(std::move(*parsed));
            }
            if (!ok) {
                continue;
            }
            rule.match = combineOr(std::move(operands));
        }

        // ── material ─────────────────────────────────────────────────────────
        if (auto mat = (*tbl)["material"].value<std::string>()) {
            rule.material = std::move(*mat);
        }

        // ── tessellation sub-table ───────────────────────────────────────────
        if (const auto *tessTbl = (*tbl)["tessellation"].as_table()) {
            warnUnknownKeys(*tessTbl, keys::kTessellationKeys, "rules.tessellation", diags);
            Rule::Tessellation tess;
            tess.skipGeometry = (*tessTbl)["skip_geometry"].value<bool>();
            tess.mergeDescendants = (*tessTbl)["merge_descendants"].value<bool>();
            tess.dropCoincidentFaces = (*tessTbl)["drop_coincident_faces"].value<bool>();
            tess.averageMaterialStack = (*tessTbl)["average_material_stack"].value<bool>();
            tess.maxSegmentsCircle = (*tessTbl)["max_segments_circle"].value<int>();
            if (auto fallbackStr = (*tessTbl)["fallback"].value<std::string>()) {
                auto parsed = parseBooleanFallback(*fallbackStr);
                if (!parsed) {
                    diags.error(codes::kErrConfigParse,
                                std::format("unknown fallback '{}'; expected skip, bbox, or fail",
                                            *fallbackStr),
                                "rules.tessellation");
                    continue;
                }
                tess.fallback = *parsed;
            }
            rule.tessellation = tess;
        }

        // ── extras sub-table ─────────────────────────────────────────────────
        if (const auto *extrasTbl = (*tbl)["extras"].as_table()) {
            rule.extras = parseExtras(*extrasTbl);
        }

        cfg.rules.push_back(std::move(rule));
    }
}

// ── Top-level parse driver ────────────────────────────────────────────────────

ConfigResult parseTable(const toml::table &tbl) {
    diagnostics::List diags;
    NHConfig cfg;

    warnUnknownKeys(tbl, keys::kTopLevelKeys, "<top-level>", diags);
    if (auto v = tbl["hoist_orphans"].value<bool>()) {
        cfg.hoistOrphans = *v;
    }
    if (auto v = tbl["deduplicate_shapes"].value<bool>()) {
        cfg.deduplicateShapes = *v;
    }
    if (const auto *exportTbl = tbl["export"].as_table()) {
        // ── Table-driven export config ───────────────────────────────────────
        // Which key each format accepts now lives in config_keys.hpp
        // (keys::kExportKeys), shared with the Lua front-end. Only the format
        // *groups* below are loader-local: they build the variant from a
        // toml::table, which Lua has no equivalent of.
        // Named static arrays so the spans below remain valid; storing an
        // initializer_list inline dangles its backing array on MSVC.
        static constexpr std::string_view kGltfGlbFormats[] = {"gltf", "glb"};
        static constexpr std::string_view kObjFormats[] = {"obj"};

        struct FormatGroup {
            std::span<const std::string_view> formats;
            ExportFormatConfig (*build)(const toml::table &, const CommonExportConfig &);
        };
        static const FormatGroup kFormatGroups[] = {
            {kGltfGlbFormats,
             [](const toml::table &t, const CommonExportConfig &c) -> ExportFormatConfig {
                 GltfExportFormatConfig out;
                 out.common = c;
                 if (auto v = t["bake_unit_scale"].value<bool>()) {
                     out.bakeUnitScale = *v;
                 }
                 if (auto v = t["multi_scene"].value<bool>()) {
                     out.multiScene = *v;
                 }
                 if (auto v = t["scene_name_separator"].value<std::string>()) {
                     out.sceneNameSeparator = std::move(*v);
                 }
                 return out;
             }},
            {kObjFormats,
             [](const toml::table &, const CommonExportConfig &c) -> ExportFormatConfig {
                 ObjExportFormatConfig out;
                 out.common = c;
                 return out;
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
            for (const auto &kd : keys::kExportKeys) {
                const bool allowed = keys::exportKeyAllowed(kd, fmtKey);
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
                    // Suppress the generic unknown-key warning below: this key is
                    // known, just misplaced, and the error above says so precisely.
                    // Without this a misplaced key is reported twice.
                    validKeys.push_back(kd.key);
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
    parseRules(tbl, cfg, diags);

    // ── [defaults] — global fallback for tessellation and extras ─────────────
    if (const auto *defTbl = tbl["defaults"].as_table()) {
        warnUnknownKeys(*defTbl, keys::kDefaultsKeys, "defaults", diags);
        if (const auto *tessTbl = (*defTbl)["tessellation"].as_table()) {
            warnUnknownKeys(*tessTbl, keys::kTessellationKeys, "defaults.tessellation", diags);
            auto &td = cfg.tessellationDefaults;
            td.skipGeometry = (*tessTbl)["skip_geometry"].value<bool>();
            td.mergeDescendants = (*tessTbl)["merge_descendants"].value<bool>();
            td.dropCoincidentFaces = (*tessTbl)["drop_coincident_faces"].value<bool>();
            td.averageMaterialStack = (*tessTbl)["average_material_stack"].value<bool>();
            td.maxSegmentsCircle = (*tessTbl)["max_segments_circle"].value<int>();
            if (auto fallbackStr = (*tessTbl)["fallback"].value<std::string>()) {
                auto parsed = parseBooleanFallback(*fallbackStr);
                if (!parsed) {
                    diags.error(codes::kErrConfigParse,
                                std::format("unknown fallback '{}'; expected skip, bbox, or fail",
                                            *fallbackStr),
                                "defaults.tessellation");
                } else {
                    td.fallback = *parsed;
                }
            }
        }
        if (const auto *extrasTbl = (*defTbl)["extras"].as_table()) {
            cfg.extrasDefaults = parseExtras(*extrasTbl);
        }
    }

    return ConfigResult{std::move(cfg), std::move(diags)};
}

// ── The filesystem include fetcher ───────────────────────────────────────────
//
// The one place includes are read from disk. Both filesystem-rooted entry
// points below share it, so "how does an include become bytes" has a single
// answer — a second fetcher here is exactly the drift that would let the two
// entry points disagree about cycles, caching, or what counts as missing.
//
// It is not itself rooted anywhere: `resolveIncludeKey` has already joined
// each key against its parent's directory by the time the fetcher sees it, so
// rooting is entirely a property of the root key the caller passes to
// `parseAndMerge`.
// The fetcher for a load that was given no base directory. It resolves
// nothing, so an include in such a config is reported as unresolvable rather
// than being looked up somewhere the caller never named — in particular, not
// against the process's working directory. Deciding that the working directory
// is the right base is an application-level choice (see cmd_config_lua.cpp,
// which makes it for the Lua front end), so it is made by the caller passing
// that directory in, never here.
IncludeFetcher unrootedFetcher() {
    return
        [](std::string_view) -> std::optional<std::span<const std::byte>> { return std::nullopt; };
}

IncludeFetcher filesystemFetcher() {
    // Cache so the fetcher returns stable spans across `parseAndMerge`'s
    // walk. Lifetime is tied to the lambda-captured `shared_ptr`, which
    // outlives the `parseAndMerge` call.
    auto cache = std::make_shared<std::unordered_map<std::string, std::vector<std::byte>>>();

    return [cache](std::string_view key) -> std::optional<std::span<const std::byte>> {
        std::string canon_key;
        std::error_code ec;
        auto canonical_path = std::filesystem::canonical(std::filesystem::path{key}, ec);
        canon_key = ec ? std::string{key} : canonical_path.string();

        if (auto it = cache->find(canon_key); it != cache->end()) {
            return std::span<const std::byte>{it->second};
        }
        try {
            auto bytes = detail::file_io::readFile(canon_key);
            auto [ins, _] = cache->emplace(std::move(canon_key), std::move(bytes));
            return std::span<const std::byte>{ins->second};
        } catch (...) {
            return std::nullopt;
        }
    };
}

} // namespace

// ── ConfigLoader ──────────────────────────────────────────────────────────────

ConfigResult ConfigLoader::collectFromFile(const std::filesystem::path &path) {
    // Read the root file and stage its bytes for `collectAndMerge`. The
    // canonical path doubles as the root key — using canonical form
    // keeps `resolveIncludeKey`'s output in the same canonical-path
    // space, so the visited set deduplicates predictably.
    //
    // A file that will not open is fatal even to the collecting face: there is
    // no document, so there is nothing to report *on*, and an empty report
    // would say the file was fine.
    std::filesystem::path canonical;
    std::vector<std::byte> root_bytes;
    try {
        canonical = std::filesystem::canonical(path);
        root_bytes = detail::file_io::readFile(canonical);
    } catch (const std::exception &e) {
        throw Error{codes::kFatalImportFileNotFound,
                    std::format("could not read config file: {}", e.what()), path.string()};
    }

    return collectAndMerge(std::span<const std::byte>{root_bytes}, canonical.string(),
                           filesystemFetcher());
}

std::vector<std::string> ConfigLoader::peekIncludesFromBytes(std::span<const std::byte> bytes) {
    std::vector<std::string> result;
    try {
        auto tbl = toml::parse(
            std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
        const auto *includeNode = tbl.get("include");
        if (includeNode == nullptr) {
            return result;
        }
        if (includeNode->is_string()) {
            result.push_back(std::string{includeNode->as_string()->get()});
        } else if (includeNode->is_array()) {
            for (const auto &e : *includeNode->as_array()) {
                if (auto s = e.value<std::string>()) {
                    result.push_back(std::move(*s));
                }
            }
        }
    } catch (...) {
        // Swallow — caller will surface the same parse error via `parseAndMerge`.
    }
    return result;
}

std::string ConfigLoader::resolveIncludeKey(std::string_view parent_key, std::string_view rel) {
    auto base = std::filesystem::path(parent_key).parent_path();
    auto joined = (base / std::filesystem::path(rel)).lexically_normal();
    return joined.generic_string();
}

ConfigResult ConfigLoader::collectAndMerge(std::span<const std::byte> root_bytes,
                                           std::string_view root_key, IncludeFetcher fetcher) {
    diagnostics::List diags;
    try {
        auto tbl = toml::parse(
            std::string_view{reinterpret_cast<const char *>(root_bytes.data()), root_bytes.size()},
            root_key);

        std::set<std::string> merged{std::string{root_key}};
        resolveIncludesFromBytes(tbl, root_key, fetcher, merged, merged, diags);
        if (diags.hasErrors()) {
            return ConfigResult{{}, std::move(diags)};
        }

        auto result = parseTable(tbl);
        diags.append(result.diags);
        result.diags = std::move(diags);
        return result;
    } catch (const toml::parse_error &e) {
        diags.error(
            codes::kErrConfigParse, e.description(),
            std::format("{}:{}:{}", root_key, e.source().begin.line, e.source().begin.column));
    }
    return ConfigResult{{}, std::move(diags)};
}

ConfigResult ConfigLoader::collectFromString(std::string_view content, std::string_view sourceName,
                                             const std::filesystem::path &baseDir) {
    // The root key is what `resolveIncludeKey` takes the parent directory of,
    // so joining `baseDir` onto the source name is what roots the include tree
    // there. An empty `baseDir` leaves the key equal to `sourceName`, which
    // also keeps the parse-error context unchanged for callers that name no
    // directory — and those get a fetcher that resolves nothing, so no include
    // is ever read from a location the caller did not choose.
    const std::string root_key = (baseDir / std::filesystem::path{sourceName}).generic_string();
    return collectAndMerge(std::as_bytes(std::span{content.data(), content.size()}), root_key,
                           baseDir.empty() ? unrootedFetcher() : filesystemFetcher());
}

// ── The faces that promise a config ──────────────────────────────────────────
//
// Each is its collecting twin plus the one decision the twin declines to make:
// that what was collected is fatal to a caller who asked for a config rather
// than for a report. `throwIfErrors` carries the whole collection on the
// exception, so nothing observed is lost by the failure being fatal.

namespace {
[[nodiscard]] ConfigResult demandConfig(ConfigResult collected, std::string_view context) {
    diagnostics::throwIfErrors(collected.diags, context);
    return collected;
}
} // namespace

ConfigResult ConfigLoader::loadFromFile(const std::filesystem::path &path) {
    return demandConfig(collectFromFile(path), path.string());
}

ConfigResult ConfigLoader::loadFromString(std::string_view content, std::string_view sourceName,
                                          const std::filesystem::path &baseDir) {
    return demandConfig(collectFromString(content, sourceName, baseDir), sourceName);
}

ConfigResult ConfigLoader::parseAndMerge(std::span<const std::byte> root_bytes,
                                         std::string_view root_key, IncludeFetcher fetcher) {
    return demandConfig(collectAndMerge(root_bytes, root_key, std::move(fetcher)), root_key);
}

} // namespace nodehammer::config
