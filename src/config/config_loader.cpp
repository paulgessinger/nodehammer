#include <nodehammer/config/config_loader.hpp>
#include <nodehammer/config/predicate_parser.hpp>
#include <nodehammer/ir/diagnostic_codes.hpp>

#include <toml++/toml.hpp>

#include <format>
#include <set>
#include <utility>

namespace nodehammer {

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

/// Recursively resolve includes in a parsed TOML table.
/// `basePath` is the directory of the file being processed.
/// `visited` tracks canonical paths for cycle detection.
void resolveIncludes(toml::table &tbl, const std::filesystem::path &basePath,
                     std::set<std::filesystem::path> &visited, DiagnosticList &diags) {
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

    // Parse and resolve all included files first, preserving order.
    std::vector<toml::table> includedTables;
    for (const auto &relPath : paths) {
        auto fullPath = std::filesystem::canonical(basePath / relPath);

        if (visited.contains(fullPath)) {
            diags.error(codes::kErrConfigParse,
                        std::format("circular include detected: '{}'", fullPath.string()),
                        "<include>");
            continue;
        }
        visited.insert(fullPath);

        toml::table included;
        try {
            included = toml::parse_file(fullPath.string());
        } catch (const toml::parse_error &e) {
            diags.error(codes::kErrConfigParse, e.description(),
                        std::format("{}:{}:{}", fullPath.string(), e.source().begin.line,
                                    e.source().begin.column));
            continue;
        } catch (const std::exception &e) {
            diags.error(codes::kErrImportFileNotFound,
                        std::format("could not read included file: {}", e.what()),
                        fullPath.string());
            continue;
        }

        resolveIncludes(included, fullPath.parent_path(), visited, diags);
        includedTables.push_back(std::move(included));
    }

    // Build a combined table from all includes in order, then merge the
    // parent's own entries on top. This gives: include1, include2, ..., parent.
    toml::table combined;
    for (auto &inc : includedTables) {
        mergeToml(combined, inc);
    }
    // Parent entries come last (fallbacks in first-match-wins).
    mergeToml(combined, tbl);
    tbl = std::move(combined);
}

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
        warnUnknownKeys(*tbl,
                        {"base_color", "metallic", "roughness", "double_sided", "emissive",
                         "alpha_mode", "alpha_cutoff", "ior", "transmission", "clearcoat",
                         "clearcoat_roughness", "anisotropy", "anisotropy_rotation", "specular",
                         "specular_color"},
                        std::format("materials.{}", key.str()), diags);
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
            // Parse hex color: "#RRGGBB" or "#RRGGBBAA"
            std::string_view hex = *colorStr;
            if (!hex.empty() && hex[0] == '#') {
                hex.remove_prefix(1);
            }
            // sRGB → linear conversion (hex colors are assumed sRGB).
            auto srgbToLinear = [](float c) -> float {
                return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            };
            auto parseHexByte = [&](std::string_view s, std::size_t offset,
                                    bool linearize = true) -> float {
                unsigned val = 0;
                for (int i = 0; i < 2; ++i) {
                    char c = s[offset + static_cast<std::size_t>(i)];
                    val <<= 4;
                    if (c >= '0' && c <= '9') {
                        val += static_cast<unsigned>(c - '0');
                    } else if (c >= 'a' && c <= 'f') {
                        val += static_cast<unsigned>(c - 'a' + 10);
                    } else if (c >= 'A' && c <= 'F') {
                        val += static_cast<unsigned>(c - 'A' + 10);
                    }
                }
                float f = static_cast<float>(val) / 255.0f;
                return linearize ? srgbToLinear(f) : f;
            };
            if (hex.size() >= 6) {
                def.baseColor.r = parseHexByte(hex, 0, true);
                def.baseColor.g = parseHexByte(hex, 2, true);
                def.baseColor.b = parseHexByte(hex, 4, true);
                if (hex.size() >= 8) {
                    def.baseColor.a = parseHexByte(hex, 6, false); // alpha is linear
                }
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
            if (*am == "opaque") {
                def.alphaMode = AlphaMode::Opaque;
            } else if (*am == "mask") {
                def.alphaMode = AlphaMode::Mask;
            } else if (*am == "blend") {
                def.alphaMode = AlphaMode::Blend;
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
                                           "— rule will be skipped",
                                           actKey),
                               "selection_rules");
                    break;
                }
                if (operands.size() == 1) {
                    pred = std::move(operands[0]);
                } else if (!operands.empty()) {
                    pred = PredicateExpr{
                        std::make_shared<OrPredicate>(OrPredicate{std::move(operands)})};
                }
                break;
            }
        }
        if (!pred) {
            diags.error(codes::kErrConfigParse,
                        "selection_rule must have either a 'keep_if' or 'drop_if' field");
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
void parseRules(const toml::table &root, NHConfig &cfg, DiagnosticList &diags) {
    const auto *arr = root["rules"].as_array();
    if (arr == nullptr) {
        return;
    }
    for (const auto &entry : *arr) {
        const auto *tbl = entry.as_table();
        if (tbl == nullptr) {
            continue;
        }

        warnUnknownKeys(*tbl, {"match", "material", "tessellation", "extras"}, "rules", diags);

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
                           "match is an empty array — rule will match nothing; "
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
            if (operands.size() == 1) {
                rule.match = std::move(operands[0]);
            } else {
                rule.match =
                    PredicateExpr{std::make_shared<OrPredicate>(OrPredicate{std::move(operands)})};
            }
        }

        // ── material ─────────────────────────────────────────────────────────
        if (auto mat = (*tbl)["material"].value<std::string>()) {
            rule.material = std::move(*mat);
        }

        // ── tessellation sub-table ───────────────────────────────────────────
        if (const auto *tessTbl = (*tbl)["tessellation"].as_table()) {
            warnUnknownKeys(
                *tessTbl, {"skip_geometry", "merge_descendants", "max_segments_circle", "fallback"},
                "rules.tessellation", diags);
            Rule::Tessellation tess;
            tess.skipGeometry = (*tessTbl)["skip_geometry"].value<bool>().value_or(false);
            tess.mergeDescendants = (*tessTbl)["merge_descendants"].value<bool>().value_or(false);
            tess.maxSegmentsCircle = (*tessTbl)["max_segments_circle"].value<int>().value_or(64);
            if (auto fallbackStr = (*tessTbl)["fallback"].value<std::string>()) {
                auto parsed = parseFallback(*fallbackStr);
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
    DiagnosticList diags;
    NHConfig cfg;

    warnUnknownKeys(
        tbl,
        {"hoist_orphans", "deduplicate_shapes", "export", "materials", "selection_rules", "rules"},
        "<top-level>", diags);
    if (auto v = tbl["hoist_orphans"].value<bool>()) {
        cfg.hoistOrphans = *v;
    }
    if (auto v = tbl["deduplicate_shapes"].value<bool>()) {
        cfg.deduplicateShapes = *v;
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
    parseRules(tbl, cfg, diags);

    return ConfigResult{std::move(cfg), std::move(diags)};
}

} // namespace

// ── ConfigLoader ──────────────────────────────────────────────────────────────

ConfigResult ConfigLoader::loadFromFile(const std::filesystem::path &path) {
    DiagnosticList diags;
    try {
        auto tbl = toml::parse_file(path.string());

        // Resolve includes before parsing config keys.
        auto canonical = std::filesystem::canonical(path);
        std::set<std::filesystem::path> visited{canonical};
        resolveIncludes(tbl, canonical.parent_path(), visited, diags);
        if (diags.hasErrors()) {
            return ConfigResult{{}, std::move(diags)};
        }

        auto result = parseTable(tbl);
        // Include diagnostics come before parse diagnostics.
        diags.append(result.diags);
        result.diags = std::move(diags);
        return result;
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
